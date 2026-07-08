#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"

using namespace clang;
using namespace clang::tooling;

class PreprocessorHooks : public PPCallbacks
{
public:
    PreprocessorHooks(SourceManager &SM) : SM(SM) {}

    void InclusionDirective(
        SourceLocation HashLoc, const Token &IncludeTok, StringRef FileName,
        bool IsAngled, CharSourceRange FilenameRange, OptionalFileEntryRef File,
        StringRef SearchPath, StringRef RelativePath, const Module *Imported,
        SrcMgr::CharacteristicKind FileType) override
    {
        if (SM.isWrittenInMainFile(HashLoc) && FileType != SrcMgr::C_User)
        {
            if (llvm::outs().tell() > LastPos)
                llvm::outs() << '\n';

            llvm::outs() << "#include <" << FileName << ">\n";
            LastPos = llvm::outs().tell();
        }
    }

private:
    SourceManager &SM;
    uint64_t LastPos = 0;
};

class Action : public PreprocessorFrontendAction
{
public:
    void ExecuteAction() override
    {
        CompilerInstance &CI = getCompilerInstance();
        Preprocessor &PP = CI.getPreprocessor();
        SourceManager &SM = CI.getSourceManager();

        PP.addPPCallbacks(std::make_unique<PreprocessorHooks>(SM));
        PP.EnterMainSourceFile();

        Token Tok;
        while (true)
        {
            PP.Lex(Tok);
            if (Tok.is(tok::eof))
                break;

            SourceLocation ExpansionLoc = SM.getExpansionLoc(Tok.getLocation());
            if (!SM.isInSystemHeader(ExpansionLoc))
                llvm::outs() << PP.getSpelling(Tok) << ' ';
        }

        llvm::outs() << '\n';
    }
};

int main(int argc, const char **argv)
{
    llvm::cl::opt<std::string> InputFile(llvm::cl::Positional, llvm::cl::Required, llvm::cl::desc("file..."));
    llvm::cl::ParseCommandLineOptions(argc, argv, "macro expander\n");
    FixedCompilationDatabase Compilations(".", {"-w"});
    ClangTool Tool(Compilations, {InputFile});
    return Tool.run(newFrontendActionFactory<Action>().get());
}
