#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/Lexer.h"
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
    Visitor(ASTContext &Ctx, Rewriter &RW) : Ctx(Ctx), RW(RW) {}

    bool TraverseImplicitCastExpr(ImplicitCastExpr *Cast)
    {
        if (!TraverseStmt(Cast->getSubExpr()))
            return false;

        CastKind CK = Cast->getCastKind();
        if (CK == CK_LValueToRValue || CK == CK_ArrayToPointerDecay ||
            CK == CK_FunctionToPointerDecay || CK == CK_NoOp)
            return true;

        SourceManager &SM = RW.getSourceMgr();
        SourceRange Range = SM.getExpansionRange(Cast->getSourceRange()).getAsRange();
        SourceLocation EndLoc = Lexer::getLocForEndOfToken(
            Range.getEnd(), 0, SM, Ctx.getLangOpts());

        std::string TypeName = Cast->getType().getAsString();
        RW.InsertTextAfter(EndLoc, ")");
        RW.InsertTextBefore(Range.getBegin(), "(" + TypeName + ")(");

        return true;
    }

private:
    ASTContext &Ctx;
    Rewriter &RW;
};

class Consumer : public ASTConsumer
{
public:
    Consumer(ASTContext &Ctx, Rewriter &RW) : Ctx(Ctx), Vis(Ctx, RW) {}

    bool HandleTopLevelDecl(DeclGroupRef Group) override
    {
        SourceManager &SM = Ctx.getSourceManager();
        for (Decl *D : Group)
        {
            if (SM.isWrittenInMainFile(D->getLocation()))
                Vis.TraverseDecl(D);
        }
        return true;
    }

private:
    ASTContext &Ctx;
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
    llvm::cl::ParseCommandLineOptions(argc, argv, "implicit to explicit cast converter\n");
    FixedCompilationDatabase Compilations(".", {"-Wno-everything", "-Wno-error", "-w"});
    ClangTool Tool(Compilations, {InputFile});
    return Tool.run(newFrontendActionFactory<Action>().get());
}
