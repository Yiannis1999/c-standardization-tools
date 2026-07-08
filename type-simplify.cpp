#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/Lexer.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace clang::tooling;

class Visitor : public RecursiveASTVisitor<Visitor>
{
public:
    Visitor(ASTContext &Ctx, Rewriter &RW) : Ctx(Ctx), RW(RW) {}

    bool TraverseDeclGroup(DeclGroupRef Group)
    {
        SourceManager &SM = Ctx.getSourceManager();
        const LangOptions &LO = Ctx.getLangOpts();
        std::vector<Decl *> Decls(Group.begin(), Group.end());
        SourceLocation GroupStart = Decls.front()->getBeginLoc();
        SourceLocation GroupEnd = Decls.back()->getEndLoc();
        llvm::SmallBitVector Targets(Decls.size());
        size_t TypedefCount = 0;
        size_t FirstTypedefIndex = 0;

        // Track simple typedefs for removal
        for (size_t i = 0; i < Decls.size(); ++i)
        {
            Decl *D = Decls[i];
            if (TypedefDecl *TD = dyn_cast<TypedefDecl>(D))
            {
                if (SM.isBeforeInTranslationUnit(D->getBeginLoc(), GroupStart))
                    GroupStart = D->getBeginLoc();

                if (TypedefCount == 0)
                    FirstTypedefIndex = i;

                TypedefCount++;
                QualType T = TD->getUnderlyingType();
                bool IsBaseTarget = T->isIntegerType() || T->isFloatingType() || T->isBooleanType();
                bool HasQuals = T.hasLocalQualifiers() || T.isConstQualified() || T.isVolatileQualified();
                if (IsBaseTarget && !HasQuals)
                    Targets.set(i);
            }
        }

        if (Targets.any())
        {
            // If all typedefs are simple, remove the entire group 
            if (Targets.count() == TypedefCount)
            {
                std::optional<Token> Semi = Lexer::findNextToken(GroupEnd, SM, LO);
                if (Semi && Semi->is(tok::semi))
                    RW.RemoveText(SourceRange(GroupStart, Semi->getLocation()));
            }
            // Otherwise, remove only the simple typedefs
            else
            {
                for (size_t i = 0; i < Decls.size();)
                {
                    if (!Targets.test(i))
                    {
                        TraverseDecl(Decls[i]);
                        i++;
                        continue;
                    }

                    size_t RunStart = i;
                    do
                        i++;
                    while (i < Decls.size() && Targets.test(i));

                    size_t RunEnd = i - 1;
                    if (RunStart == FirstTypedefIndex)
                    {
                        std::optional<Token> Comma = Lexer::findNextToken(Decls[RunEnd]->getEndLoc(), SM, LO);
                        if (Comma && Comma->is(tok::comma))
                            RW.RemoveText(CharSourceRange::getTokenRange(Decls[RunStart]->getLocation(), Comma->getLocation()));
                    }
                    else
                    {
                        std::optional<Token> Comma = Lexer::findNextToken(Decls[RunStart - 1]->getEndLoc(), SM, LO);
                        if (Comma && Comma->is(tok::comma))
                            RW.RemoveText(CharSourceRange::getTokenRange(Comma->getLocation(), Decls[RunEnd]->getEndLoc()));
                    }
                }
            }
        }
        else
        {
            for (Decl *D : Decls)
                TraverseDecl(D);
        }

        return true;
    }

    bool TraverseDeclStmt(DeclStmt *S)
    {
        if (S->isSingleDecl())
            TraverseDeclGroup(DeclGroupRef(S->getSingleDecl()));
        else
            TraverseDeclGroup(S->getDeclGroup());

        return true;
    }

    bool TraverseVarDecl(VarDecl *VD)
    {
        if (!RecursiveASTVisitor<Visitor>::TraverseVarDecl(VD))
            return false;

        // Hide the initializer to prevent constant folding of references
        if (VD->hasInit())
            VD->setInit(nullptr);

        return true;
    }

    bool TraverseTypeLoc(TypeLoc TL)
    {
        if (TL.getTypeLocClass() == TypeLoc::TypeOfExpr || TL.getTypeLocClass() == TypeLoc::TypeOf ||
            TL.getTypeLocClass() == TypeLoc::Decltype || TL.getTypeLocClass() == TypeLoc::Typedef ||
            TL.getTypeLocClass() == TypeLoc::Builtin)
        {
            QualType QT = TL.getType();
            std::string Replacement;

            // Map arithmetic types to their fundamental equivalents
            if (!QT.isNull() && !QT->isIncompleteType())
            {
                QualType Canonical = QT.getCanonicalType();
                if (Canonical->isBooleanType())
                    Replacement = "char";
                else if (Canonical->isIntegerType())
                {
                    uint64_t Width = Ctx.getTypeInfo(Canonical).Width;
                    bool IsSigned = Canonical->isSignedIntegerType();
                    if (Width == 8)
                        Replacement = IsSigned ? "char" : "unsigned char";
                    else if (Width == 16)
                        Replacement = IsSigned ? "short" : "unsigned short";
                    else if (Width == 32)
                        Replacement = IsSigned ? "int" : "unsigned int";
                    else if (Width == 64)
                        Replacement = IsSigned ? "long" : "unsigned long";
                }
                else if (Canonical->isFloatingType())
                {
                    uint64_t Width = Ctx.getTypeInfo(Canonical).Width;
                    if (Width == 32)
                        Replacement = "float";
                    else if (Width == 64)
                        Replacement = "double";
                    else if (Width == 80 || Width == 128)
                        Replacement = "long double";
                }
            }

            // Resolve type inference constructs
            if (Replacement.empty() && (TL.getTypeLocClass() == TypeLoc::TypeOfExpr ||
                                        TL.getTypeLocClass() == TypeLoc::TypeOf ||
                                        TL.getTypeLocClass() == TypeLoc::Decltype))
            {
                if (!QT.isNull())
                    Replacement = QT.getCanonicalType().getAsString(Ctx.getPrintingPolicy());
            }

            if (!Replacement.empty())
            {
                if (VisitedLocs.insert(TL.getBeginLoc()).second)
                {
                    SourceManager &SM = RW.getSourceMgr();
                    RW.ReplaceText(SM.getExpansionRange(TL.getSourceRange()), Replacement);
                }
                return true;
            }
        }

        return RecursiveASTVisitor<Visitor>::TraverseTypeLoc(TL);
    }

    bool TraverseStmt(Stmt *S)
    {
        if (!S)
            return true;

        if (Expr *E = dyn_cast<Expr>(S))
            return TraverseExpr(E);

        return RecursiveASTVisitor<Visitor>::TraverseStmt(S);
    }

    bool TraverseExpr(Expr *E)
    {
        std::string Replacement;

        // Resolve compile-time arithmetic and standardize literals
        if (!E->isGLValue() && E->getType()->isArithmeticType())
        {
            Expr::EvalResult ER;
            if (E->EvaluateAsRValue(ER, Ctx))
            {
                const APValue &Val = ER.Val;
                if (Val.isInt())
                {
                    const llvm::APSInt &I = Val.getInt();
                    unsigned Width = I.getBitWidth();
                    if (Width == 8 || E->getType()->isBooleanType())
                    {
                        char C = static_cast<char>(I.getZExtValue());
                        std::string Buf;
                        llvm::raw_string_ostream OS(Buf);
                        OS << "'";
                        OS.write_escaped(llvm::StringRef(&C, 1));
                        OS << "'";
                        Replacement = OS.str();
                    }
                    else
                    {
                        llvm::SmallString<64> Buf;
                        I.toString(Buf, 10, I.isSigned());
                        Replacement = Buf.str().str();
                        if (Width == 16)
                            Replacement = (I.isSigned() ? "(short)" : "(unsigned short)") + Replacement;
                        else
                        {
                            if (I.isUnsigned())
                                Replacement += "U";
                            if (Width == 64)
                                Replacement += "LL";
                        }
                    }
                }
                else if (Val.isFloat())
                {
                    llvm::APFloat F = Val.getFloat();
                    const llvm::fltSemantics &S = F.getSemantics();

                    // Sanitize NaN / Inf
                    if (F.isNaN())
                        F = llvm::APFloat::getZero(S);
                    else if (F.isInfinity())
                        F = llvm::APFloat::getLargest(S, F.isNegative());

                    FloatingLiteral *FL = FloatingLiteral::Create(
                        Ctx, F, true, E->getType(), E->getExprLoc());
                    std::string Buf;
                    llvm::raw_string_ostream OS(Buf);
                    FL->printPretty(OS, nullptr, Ctx.getPrintingPolicy());
                    Replacement = OS.str();
                }
            }
        }
        // Truncate and standardize string literals
        else if (StringLiteral *SL = dyn_cast<StringLiteral>(E))
        {
            StringRef Bytes = SL->getBytes();
            size_t CharByteWidth = SL->getCharByteWidth();
            size_t TruncatedSize = Bytes.size();
            for (size_t i = 0; i + CharByteWidth <= Bytes.size(); i += CharByteWidth)
            {
                if (llvm::all_of(Bytes.slice(i, i + CharByteWidth), [](char C)
                                 { return C == 0; }))
                {
                    TruncatedSize = i;
                    break;
                }
            }

            SourceLocation Loc = SL->getBeginLoc();
            StringLiteral *NewSL = StringLiteral::Create(
                Ctx, Bytes.substr(0, TruncatedSize), SL->getKind(),
                SL->isPascal(), SL->getType(), &Loc, 1);

            std::string Buf;
            llvm::raw_string_ostream OS(Buf);
            NewSL->printPretty(OS, nullptr, Ctx.getPrintingPolicy());
            Replacement = OS.str();
        }

        if (!Replacement.empty())
        {
            if (VisitedLocs.insert(E->getExprLoc()).second)
            {
                SourceManager &SM = RW.getSourceMgr();
                RW.ReplaceText(SM.getExpansionRange(E->getSourceRange()), Replacement);
            }
            return true;
        }

        return RecursiveASTVisitor<Visitor>::TraverseStmt(E);
    }

private:
    ASTContext &Ctx;
    Rewriter &RW;
    std::set<SourceLocation> VisitedLocs;
};

class Consumer : public ASTConsumer
{
public:
    Consumer(ASTContext &Ctx, Rewriter &RW) : Ctx(Ctx), Vis(Ctx, RW) {}

    bool HandleTopLevelDecl(DeclGroupRef Group) override
    {
        SourceLocation Loc = Ctx.getSourceManager().getExpansionLoc((*Group.begin())->getLocation());
        if (Ctx.getSourceManager().isWrittenInMainFile(Loc))
            return Vis.TraverseDeclGroup(Group);

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
    llvm::cl::ParseCommandLineOptions(argc, argv, "type and literal simplifier\n");
    FixedCompilationDatabase Compilations(".", {"-Wno-everything", "-Wno-error", "-w"});
    ClangTool Tool(Compilations, {InputFile});
    return Tool.run(newFrontendActionFactory<Action>().get());
}
