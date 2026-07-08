#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace clang::tooling;

class Visitor : public RecursiveASTVisitor<Visitor>
{
public:
    Visitor(ASTContext &Ctx) : Ctx(Ctx) {}

    bool shouldUseDataRecursionFor(Stmt *S) const
    {
        return false;
    }

    bool TraverseDecl(Decl *D)
    {
        if (!D)
            return true;

        Ancestors.push_back(DynTypedNode::create(*D));
        bool Result = RecursiveASTVisitor<Visitor>::TraverseDecl(D);
        Ancestors.pop_back();

        return Result;
    }

    bool TraverseStmt(Stmt *S)
    {
        if (!S)
            return true;

        Ancestors.push_back(DynTypedNode::create(*S));
        bool Result = RecursiveASTVisitor<Visitor>::TraverseStmt(S);
        Ancestors.pop_back();

        return Result;
    }

    bool TraverseBinaryOperator(BinaryOperator *BO)
    {
        if (!RecursiveASTVisitor<Visitor>::TraverseBinaryOperator(BO))
            return false;

        if (BO->getOpcode() == BO_Add)
        {
            Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
            if (UnaryOperator *UO = dyn_cast<UnaryOperator>(LHS))
            {
                // -A + B  ->  B - A
                if (UO->getOpcode() == UO_Minus)
                {
                    Expr *A = UO->getSubExpr();
                    Expr *B = BO->getRHS();
                    BO->setOpcode(BO_Sub);
                    BO->setLHS(B);
                    BO->setRHS(A);
                }
            }
        }

        return true;
    }

    bool TraverseUnaryOperator(UnaryOperator *UO)
    {
        if (!RecursiveASTVisitor<Visitor>::TraverseUnaryOperator(UO))
            return false;

        if (UO->getOpcode() == UO_Minus)
        {
            Expr *DirectChild = UO->getSubExpr();
            Expr *LogicalChild = DirectChild->IgnoreParenImpCasts();
            if (BinaryOperator *BO = dyn_cast<BinaryOperator>(LogicalChild))
            {
                // -(A - B) -> (B - A)
                if (BO->getOpcode() == BO_Sub)
                {
                    Expr *A = BO->getLHS();
                    Expr *B = BO->getRHS();
                    BO->setLHS(B);
                    BO->setRHS(A);
                    replaceCurrentStmt(DirectChild);
                }
            }
        }

        return true;
    }

private:
    ASTContext &Ctx;
    std::vector<DynTypedNode> Ancestors;

    void replaceCurrentStmt(Stmt *Replacement)
    {
        const Stmt *Target = Ancestors.back().get<Stmt>();
        const DynTypedNode &Parent = Ancestors[Ancestors.size() - 2];

        if (const Stmt *ParentStmt = Parent.get<Stmt>())
        {
            for (Stmt *&Child : const_cast<Stmt *>(ParentStmt)->children())
            {
                if (Child == Target)
                {
                    Child = Replacement;
                    return;
                }
            }

            return;
        }

        if (const VarDecl *VD = Parent.get<VarDecl>())
        {
            if (VD->getInit() == Target)
            {
                if (Expr *ReplacementExpr = dyn_cast<Expr>(Replacement))
                    const_cast<VarDecl *>(VD)->setInit(ReplacementExpr);
            }
        }
    }
};

class Consumer : public ASTConsumer
{
public:
    Consumer(ASTContext &Ctx, Rewriter &RW) : Ctx(Ctx), RW(RW), Vis(Ctx) {}

    bool HandleTopLevelDecl(DeclGroupRef Group) override
    {
        SourceManager &SM = Ctx.getSourceManager();
        for (Decl *D : Group)
        {
            if (FunctionDecl *FD = dyn_cast<FunctionDecl>(D))
            {
                if (SM.isWrittenInMainFile(FD->getLocation()))
                {
                    Vis.TraverseDecl(FD);
                    std::string Buf;
                    {
                        llvm::raw_string_ostream Out(Buf);
                        FD->print(Out);
                    }
                    RW.ReplaceText(SM.getExpansionRange(FD->getSourceRange()), Buf);
                }
            }
        }
        return true;
    }

private:
    ASTContext &Ctx;
    Rewriter &RW;
    Visitor Vis;
};

class Action : public ASTFrontendAction
{
public:
    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI, llvm::StringRef) override
    {
        RW.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
        return std::make_unique<Consumer>(CI.getASTContext(), RW);
    }

    void EndSourceFileAction() override
    {
        FileID MainFileID = RW.getSourceMgr().getMainFileID();
        RW.getEditBuffer(MainFileID).write(llvm::outs());
    }

private:
    Rewriter RW;
};

int main(int argc, const char **argv)
{
    llvm::cl::opt<std::string> InputFile(llvm::cl::Positional, llvm::cl::Required, llvm::cl::desc("file..."));
    llvm::cl::ParseCommandLineOptions(argc, argv, "unary operator simplifier\n");
    FixedCompilationDatabase Compilations(".", {"-Wno-everything", "-Wno-error", "-w"});
    ClangTool Tool(Compilations, {InputFile});
    return Tool.run(newFrontendActionFactory<Action>().get());
}
