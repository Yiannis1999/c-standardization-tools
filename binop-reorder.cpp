#include "clang/AST/ASTConsumer.h"
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

    bool VisitBinaryOperator(BinaryOperator *BO)
    {
        BinaryOperatorKind Kind = BO->getOpcode();
        Expr *LHS = BO->getLHS();
        Expr *RHS = BO->getRHS();

        if (!LHS->getType()->isArithmeticType() || !RHS->getType()->isArithmeticType())
            return true;

        if (!RHS->isEvaluatable(Ctx) || LHS->isEvaluatable(Ctx))
            return true;

        if (Kind == BO_Add || Kind == BO_Mul)
        {
            BO->setLHS(RHS);
            BO->setRHS(LHS);
        }
        else if (Kind == BO_Sub)
        {
            UnaryOperator *Neg = UnaryOperator::Create(
                Ctx, RHS, UO_Minus, RHS->getType(), RHS->getValueKind(),
                RHS->getObjectKind(), BO->getOperatorLoc(), false, FPOptionsOverride());
            BO->setOpcode(BO_Add);
            BO->setLHS(Neg);
            BO->setRHS(LHS);
        }

        return true;
    }

private:
    ASTContext &Ctx;
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
    llvm::cl::ParseCommandLineOptions(argc, argv, "binary operator normalizer\n");
    FixedCompilationDatabase Compilations(".", {"-Wno-everything", "-Wno-error", "-w"});
    ClangTool Tool(Compilations, {InputFile});
    return Tool.run(newFrontendActionFactory<Action>().get());
}
