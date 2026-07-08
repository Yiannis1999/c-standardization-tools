#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"

using namespace clang;
using namespace clang::tooling;

static bool LargeInit = false;

class Visitor : public RecursiveASTVisitor<Visitor>
{
public:
    Visitor(ASTContext &Ctx) : Ctx(Ctx) {}

    bool VisitVarDecl(VarDecl *VD)
    {
        if (!VD->hasInit())
            return true;

        QualType T = VD->getType();
        if (T->isIncompleteType() || T->isReferenceType())
            return true;

        if (Ctx.getTypeSize(T) > 2048) // 2048 bits = 256 bytes
            LargeInit = true;

        return true;
    }

private:
    ASTContext &Ctx;
};

class Consumer : public ASTConsumer
{
public:
    Consumer(ASTContext &Ctx) : Ctx(Ctx), Vis(Ctx) {}

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
        return std::make_unique<Consumer>(CI.getASTContext());
    }
};

int main(int argc, const char **argv)
{
    llvm::cl::opt<std::string> InputFile(llvm::cl::Positional, llvm::cl::Required, llvm::cl::desc("file..."));
    llvm::cl::ParseCommandLineOptions(argc, argv, "large initializer detector\n");
    FixedCompilationDatabase Compilations(".", {"-Wno-everything", "-Wno-error", "-w"});
    ClangTool Tool(Compilations, {InputFile});

    int Result = Tool.run(newFrontendActionFactory<Action>().get());
    if (LargeInit)
        return 1;

    return Result;
}
