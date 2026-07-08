#include <cmath>
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APSInt.h"
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

    bool TraverseGenericSelectionExpr(GenericSelectionExpr *E)
    {
        if (!RecursiveASTVisitor<Visitor>::TraverseGenericSelectionExpr(E))
            return false;

        Expr *Selected = E->getResultExpr();

        // _Generic(..., T: expr, ...) -> expr
        if (Selected)
            replaceCurrentStmt(Selected);

        return true;
    }

    bool TraverseBinaryOperator(BinaryOperator *BO)
    {
        if (!RecursiveASTVisitor<Visitor>::TraverseBinaryOperator(BO))
            return false;

        BinaryOperatorKind Kind = BO->getOpcode();
        Expr *LHS = BO->getLHS();
        Expr *RHS = BO->getRHS();

        switch (Kind)
        {
        case BO_Add:
            // x + 0 -> x
            if (matchesValue(RHS, 0.0))
            {
                replaceCurrentStmt(LHS);
                return true;
            }

            // 0 + x -> x
            if (matchesValue(LHS, 0.0))
            {
                replaceCurrentStmt(RHS);
                return true;
            }
            break;

        case BO_Sub:
            // x - 0 -> x
            if (matchesValue(RHS, 0.0))
            {
                replaceCurrentStmt(LHS);
                return true;
            }

            // 0 - x -> -x
            if (matchesValue(LHS, 0.0))
            {
                replaceCurrentStmt(createUnaryNeg(RHS));
                return true;
            }

            // x - x -> 0
            if (isSameExpr(LHS, RHS))
            {
                replaceCurrentStmt(createInt(0));
                return true;
            }
            break;

        case BO_Mul:
            // x * 1 -> x
            if (matchesValue(RHS, 1.0))
            {
                replaceCurrentStmt(LHS);
                return true;
            }

            // 1 * x -> x
            if (matchesValue(LHS, 1.0))
            {
                replaceCurrentStmt(RHS);
                return true;
            }

            // x * 0 -> 0
            if (matchesValue(RHS, 0.0))
            {
                replaceCurrentStmt(RHS);
                return true;
            }

            // 0 * x -> 0
            if (matchesValue(LHS, 0.0))
            {
                replaceCurrentStmt(LHS);
                return true;
            }

            // x * -1 -> -x
            if (matchesValue(RHS, -1.0))
            {
                replaceCurrentStmt(createUnaryNeg(LHS));
                return true;
            }

            // -1 * x -> -x
            if (matchesValue(LHS, -1.0))
            {
                replaceCurrentStmt(createUnaryNeg(RHS));
                return true;
            }
            break;

        case BO_Div:
            // x / 1 -> x
            if (matchesValue(RHS, 1.0))
            {
                replaceCurrentStmt(LHS);
                return true;
            }

            // x / x -> 1
            if (isSameExpr(LHS, RHS) && !isZero(RHS))
            {
                replaceCurrentStmt(createInt(1));
                return true;
            }
            break;

        case BO_Rem:
            // x % 1 -> 0
            // x % -1 -> 0
            if (matchesValue(RHS, 1.0) || matchesValue(RHS, -1.0))
            {
                replaceCurrentStmt(createInt(0));
                return true;
            }
            break;

        default:
            break;
        }

        return true;
    }

    bool TraverseCompoundAssignOperator(CompoundAssignOperator *CAO)
    {
        if (!RecursiveASTVisitor<Visitor>::TraverseCompoundAssignOperator(CAO))
            return false;

        BinaryOperatorKind Kind = CAO->getOpcode();
        Expr *LHS = CAO->getLHS();
        Expr *RHS = CAO->getRHS();

        if (Kind == BO_AddAssign || Kind == BO_SubAssign)
        {
            // x += 0 -> x
            // x -= 0 -> x
            if (matchesValue(RHS, 0.0))
            {
                replaceCurrentStmt(LHS);
                return true;
            }
        }
        else if (Kind == BO_MulAssign)
        {
            // x *= 1 -> x
            if (matchesValue(RHS, 1.0))
            {
                replaceCurrentStmt(LHS);
                return true;
            }

            // x *= 0 -> x = 0
            if (matchesValue(RHS, 0.0))
            {
                BinaryOperator *Assign = BinaryOperator::Create(
                    Ctx, LHS, RHS, BO_Assign, CAO->getType(), CAO->getValueKind(),
                    CAO->getObjectKind(), CAO->getOperatorLoc(), FPOptionsOverride());
                replaceCurrentStmt(Assign);
                return true;
            }
        }
        else if (Kind == BO_DivAssign)
        {
            // x /= 1 -> x
            if (matchesValue(RHS, 1.0))
            {
                replaceCurrentStmt(LHS);
                return true;
            }
        }

        return true;
    }

    bool TraverseCallExpr(CallExpr *E)
    {
        if (!RecursiveASTVisitor<Visitor>::TraverseCallExpr(E))
            return false;

        FunctionDecl *FD = E->getDirectCallee();
        if (!FD)
            return true;

        StringRef Name = FD->getName();
        if (Name.starts_with("__builtin_"))
            Name = Name.drop_front(10);

        unsigned N = E->getNumArgs();
        Expr *A0 = (N > 0) ? E->getArg(0) : nullptr;
        Expr *A1 = (N > 1) ? E->getArg(1) : nullptr;
        Expr *A2 = (N > 2) ? E->getArg(2) : nullptr;

        if (N == 1)
        {
            double Val;
            if (getFloatValue(A0, Val))
            {
                // log(const) -> result
                if (Name == "log" || Name == "logf" || Name == "logl")
                {
                    replaceCurrentStmt(createFloat(std::log(Val)));
                    return true;
                }

                // log2(const) -> result
                if (Name == "log2" || Name == "log2f" || Name == "log2l")
                {
                    replaceCurrentStmt(createFloat(std::log2(Val)));
                    return true;
                }

                // log10(const) -> result
                if (Name == "log10" || Name == "log10f" || Name == "log10l")
                {
                    replaceCurrentStmt(createFloat(std::log10(Val)));
                    return true;
                }

                // log1p(const) -> result
                if (Name == "log1p" || Name == "log1pf" || Name == "log1pl")
                {
                    replaceCurrentStmt(createFloat(std::log1p(Val)));
                    return true;
                }

                // exp(const) -> result
                if (Name == "exp" || Name == "expf" || Name == "expl")
                {
                    replaceCurrentStmt(createFloat(std::exp(Val)));
                    return true;
                }

                // exp2(const) -> result
                if (Name == "exp2" || Name == "exp2f" || Name == "exp2l")
                {
                    replaceCurrentStmt(createFloat(std::exp2(Val)));
                    return true;
                }

                // expm1(const) -> result
                if (Name == "expm1" || Name == "expm1f" || Name == "expm1l")
                {
                    replaceCurrentStmt(createFloat(std::expm1(Val)));
                    return true;
                }

                // sin(const) -> result
                if (Name == "sin" || Name == "sinf" || Name == "sinl")
                {
                    replaceCurrentStmt(createFloat(std::sin(Val)));
                    return true;
                }

                // cos(const) -> result
                if (Name == "cos" || Name == "cosf" || Name == "cosl")
                {
                    replaceCurrentStmt(createFloat(std::cos(Val)));
                    return true;
                }

                // tan(const) -> result
                if (Name == "tan" || Name == "tanf" || Name == "tanl")
                {
                    replaceCurrentStmt(createFloat(std::tan(Val)));
                    return true;
                }

                // asin(const) -> result
                if (Name == "asin" || Name == "asinf" || Name == "asinl")
                {
                    replaceCurrentStmt(createFloat(std::asin(Val)));
                    return true;
                }

                // acos(const) -> result
                if (Name == "acos" || Name == "acosf" || Name == "acosl")
                {
                    replaceCurrentStmt(createFloat(std::acos(Val)));
                    return true;
                }

                // atan(const) -> result
                if (Name == "atan" || Name == "atanf" || Name == "atanl")
                {
                    replaceCurrentStmt(createFloat(std::atan(Val)));
                    return true;
                }

                // sinh(const) -> result
                if (Name == "sinh" || Name == "sinhf" || Name == "sinhl")
                {
                    replaceCurrentStmt(createFloat(std::sinh(Val)));
                    return true;
                }

                // cosh(const) -> result
                if (Name == "cosh" || Name == "coshf" || Name == "coshl")
                {
                    replaceCurrentStmt(createFloat(std::cosh(Val)));
                    return true;
                }

                // tanh(const) -> result
                if (Name == "tanh" || Name == "tanhf" || Name == "tanhl")
                {
                    replaceCurrentStmt(createFloat(std::tanh(Val)));
                    return true;
                }

                // sqrt(const) -> result
                if (Name == "sqrt" || Name == "sqrtf" || Name == "sqrtl")
                {
                    replaceCurrentStmt(createFloat(std::sqrt(Val)));
                    return true;
                }

                // cbrt(const) -> result
                if (Name == "cbrt" || Name == "cbrtf" || Name == "cbrtl")
                {
                    replaceCurrentStmt(createFloat(std::cbrt(Val)));
                    return true;
                }

                // fabs(const) -> result
                if (Name == "fabs" || Name == "fabsf" || Name == "fabsl")
                {
                    replaceCurrentStmt(createFloat(std::fabs(Val)));
                    return true;
                }

                // ceil(const) -> result
                if (Name == "ceil" || Name == "ceilf" || Name == "ceill")
                {
                    replaceCurrentStmt(createFloat(std::ceil(Val)));
                    return true;
                }

                // floor(const) -> result
                if (Name == "floor" || Name == "floorf" || Name == "floorl")
                {
                    replaceCurrentStmt(createFloat(std::floor(Val)));
                    return true;
                }

                // round(const) -> result
                if (Name == "round" || Name == "roundf" || Name == "roundl")
                {
                    replaceCurrentStmt(createFloat(std::round(Val)));
                    return true;
                }

                // trunc(const) -> result
                if (Name == "trunc" || Name == "truncf" || Name == "truncl")
                {
                    replaceCurrentStmt(createFloat(std::trunc(Val)));
                    return true;
                }
            }

            llvm::APSInt IntVal;
            if (getIntValue(A0, IntVal) && IntVal.isRepresentableByInt64())
            {
                int64_t ExtVal = IntVal.getExtValue();

                // abs(const_int) -> result
                if (Name == "abs" || Name == "labs" || Name == "llabs")
                {
                    replaceCurrentStmt(createInt(std::abs(ExtVal)));
                    return true;
                }

                // toascii(const) -> result
                if (Name == "toascii")
                {
                    replaceCurrentStmt(createInt(ExtVal & 0x7F));
                    return true;
                }

                // ffs(const) -> result
                if (Name == "ffs")
                {
                    replaceCurrentStmt(createInt(__builtin_ffsll(ExtVal)));
                    return true;
                }
            }
        }

        if (N == 2)
        {
            double Val0, Val1;
            if (getFloatValue(A0, Val0) && getFloatValue(A1, Val1))
            {
                // atan2(const, const) -> result
                if (Name == "atan2" || Name == "atan2f" || Name == "atan2l")
                {
                    replaceCurrentStmt(createFloat(std::atan2(Val0, Val1)));
                    return true;
                }

                // pow(const, const) -> result
                if (Name == "pow" || Name == "powf" || Name == "powl")
                {
                    replaceCurrentStmt(createFloat(std::pow(Val0, Val1)));
                    return true;
                }

                // fmod(const, const) -> result
                if (Name == "fmod" || Name == "fmodf" || Name == "fmodl")
                {
                    replaceCurrentStmt(createFloat(std::fmod(Val0, Val1)));
                    return true;
                }

                // hypot(const, const) -> result
                if (Name == "hypot" || Name == "hypotf" || Name == "hypotl")
                {
                    replaceCurrentStmt(createFloat(std::hypot(Val0, Val1)));
                    return true;
                }

                // fmin(const, const) -> result
                if (Name == "fmin" || Name == "fminf" || Name == "fminl")
                {
                    replaceCurrentStmt(createFloat(std::fmin(Val0, Val1)));
                    return true;
                }

                // fmax(const, const) -> result
                if (Name == "fmax" || Name == "fmaxf" || Name == "fmaxl")
                {
                    replaceCurrentStmt(createFloat(std::fmax(Val0, Val1)));
                    return true;
                }

                // copysign(const, const) -> result
                if (Name == "copysign" || Name == "copysignf" || Name == "copysignl")
                {
                    replaceCurrentStmt(createFloat(std::copysign(Val0, Val1)));
                    return true;
                }

                // remainder(const, const) -> result
                if (Name == "remainder" || Name == "remainderf" || Name == "remainderl")
                {
                    replaceCurrentStmt(createFloat(std::remainder(Val0, Val1)));
                    return true;
                }
            }
        }

        if (Name == "strcat" && N >= 2)
        {
            if (const StringLiteral *SL = getStringLiteral(A1))
            {
                // strcat(dest, "") -> dest
                if (SL->getLength() == 0)
                {
                    replaceCurrentStmt(A0);
                    return true;
                }
            }
        }

        if (Name == "strcpy" && N >= 2)
        {
            // strcpy(x, x) -> x
            if (isSameExpr(A0, A1))
            {
                replaceCurrentStmt(A0);
                return true;
            }
        }

        if (Name == "strcmp" && N >= 2)
        {
            // strcmp(x, x) -> 0
            if (isSameExpr(A0, A1))
            {
                replaceCurrentStmt(createInt(0));
                return true;
            }

            if (const StringLiteral *SL = getStringLiteral(A1))
            {
                // strcmp(s, "") -> (unsigned char)*s
                if (SL->getLength() == 0)
                {
                    replaceCurrentStmt(createCast(Ctx.UnsignedCharTy, createDeref(A0)));
                    return true;
                }
            }
        }

        if (Name == "strstr" && N >= 2)
        {
            if (const StringLiteral *SL = getStringLiteral(A1))
            {
                // strstr(s, "") -> s
                if (SL->getLength() == 0)
                {
                    replaceCurrentStmt(A0);
                    return true;
                }

                // strstr(s, "c") -> strchr(s, 'c')
                if (SL->getLength() == 1)
                {
                    if (CallExpr *StrChr = createCall("strchr", {A0, createInt(SL->getString()[0])}))
                    {
                        replaceCurrentStmt(StrChr);
                        return true;
                    }
                }
            }
        }

        if (Name == "strpbrk" && N >= 2)
        {
            if (const StringLiteral *SL = getStringLiteral(A1))
            {
                // strpbrk(s, "") -> (char*)0
                if (SL->getLength() == 0)
                {
                    replaceCurrentStmt(createCast(E->getType(), createInt(0)));
                    return true;
                }
            }
        }

        if (Name == "strspn" && N >= 2)
        {
            if (const StringLiteral *SL = getStringLiteral(A1))
            {
                // strspn(s, "") -> 0
                if (SL->getLength() == 0)
                {
                    replaceCurrentStmt(createInt(0));
                    return true;
                }
            }
        }

        if (Name == "strcspn" && N >= 2)
        {
            if (const StringLiteral *SL = getStringLiteral(A1))
            {
                // strcspn(s, "") -> strlen(s)
                if (SL->getLength() == 0)
                {
                    if (CallExpr *StrLen = createCall("strlen", {A0}))
                    {
                        replaceCurrentStmt(StrLen);
                        return true;
                    }
                }
            }
        }

        // bcmp(s1, s2, n) -> memcmp(s1, s2, n)
        if (Name == "bcmp" && N == 3)
        {
            if (CallExpr *MemCmp = createCall("memcmp", {A0, A1, A2}))
            {
                replaceCurrentStmt(MemCmp);
                return true;
            }
        }

        if (N >= 3 && isZero(A2))
        {
            // strncmp(s1, s2, 0) -> 0
            // memcmp(s1, s2, 0) -> 0
            // strncasecmp(s1, s2, 0) -> 0
            if (Name == "strncmp" || Name == "memcmp" || Name == "strncasecmp")
            {
                replaceCurrentStmt(createInt(0));
                return true;
            }

            // memchr(s, c, 0) -> nullptr
            if (Name == "memchr")
            {
                replaceCurrentStmt(createCast(E->getType(), createInt(0)));
                return true;
            }

            // memset(s, c, 0) -> s
            // strncat(s1, s2, 0) -> s1
            // memmove(s1, s2, 0) -> s1
            if (Name == "memset" || Name == "strncat" || Name == "memmove")
            {
                replaceCurrentStmt(A0);
                return true;
            }
        }

        if (N >= 2 && isSameExpr(A0, A1))
        {
            // memcmp(x, x, n) -> 0
            // strncmp(x, x, n) -> 0
            // strcasecmp(x, x) -> 0
            // strncasecmp(x, x, n) -> 0
            if (Name == "memcmp" || Name == "strncmp" || Name == "strcasecmp" || Name == "strncasecmp")
            {
                replaceCurrentStmt(createInt(0));
                return true;
            }

            // memmove(x, x, n) -> x
            // memcpy(x, x, n) -> x
            if (Name == "memmove" || Name == "memcpy")
            {
                replaceCurrentStmt(A0);
                return true;
            }

            // fmin(x, x) -> x
            // fmax(x, x) -> x
            // copysign(x, x) -> x
            if (Name == "fmin" || Name == "fminf" || Name == "fminl" ||
                Name == "fmax" || Name == "fmaxf" || Name == "fmaxl" ||
                Name == "copysign" || Name == "copysignf" || Name == "copysignl")
            {
                replaceCurrentStmt(A0);
                return true;
            }
        }

        if ((Name == "memcmp" || Name == "strncmp") && N == 3)
        {
            llvm::APSInt Len;

            // memcmp(a, b, 1) -> (unsigned char)*a - (unsigned char)*b
            if (getIntValue(A2, Len) && Len == 1)
            {
                Expr *CastA = createCast(Ctx.UnsignedCharTy, createDeref(A0));
                Expr *CastB = createCast(Ctx.UnsignedCharTy, createDeref(A1));
                Expr *Sub = BinaryOperator::Create(
                    Ctx, CastA, CastB, BO_Sub, Ctx.IntTy, VK_PRValue,
                    OK_Ordinary, SourceLocation(), FPOptionsOverride());
                replaceCurrentStmt(Sub);
                return true;
            }
        }

        // strchr(s, 0) -> s + strlen(s)
        if ((Name == "strchr" || Name == "strrchr") && N == 2 && isZero(A1))
        {
            if (CallExpr *StrLen = createCall("strlen", {A0}))
            {
                Expr *Add = BinaryOperator::Create(
                    Ctx, A0, StrLen, BO_Add, E->getType(), VK_PRValue,
                    OK_Ordinary, SourceLocation(), FPOptionsOverride());
                replaceCurrentStmt(Add);
                return true;
            }
        }

        if (Name == "printf")
        {
            const StringLiteral *SL = getStringLiteral(A0);
            if (SL)
            {
                StringRef Fmt = SL->getString();

                // printf("") -> 0
                if (N == 1 && Fmt.empty())
                {
                    replaceCurrentStmt(createInt(0));
                    return true;
                }

                // printf("c") -> putchar('c')
                if (N == 1 && Fmt.size() == 1 && !Fmt.contains('%'))
                {
                    if (CallExpr *PutC = createCall("putchar", {createChar(Fmt[0])}))
                    {
                        replaceCurrentStmt(PutC);
                        return true;
                    }
                }

                // printf("str\n") -> puts("str")
                if (N == 1 && Fmt.ends_with("\n") && !Fmt.contains('%'))
                {
                    if (CallExpr *Puts = createCall("puts", {createString(Fmt.drop_back(1))}))
                    {
                        replaceCurrentStmt(Puts);
                        return true;
                    }
                }

                // printf("%s\n", str) -> puts(str)
                if (N == 2 && Fmt == "%s\n")
                {
                    if (CallExpr *Puts = createCall("puts", {A1}))
                    {
                        replaceCurrentStmt(Puts);
                        return true;
                    }
                }

                // printf("%c", ch) -> putchar(ch)
                if (N == 2 && Fmt == "%c")
                {
                    if (CallExpr *PutC = createCall("putchar", {A1}))
                    {
                        replaceCurrentStmt(PutC);
                        return true;
                    }
                }
            }
        }

        if (Name == "fprintf")
        {
            const StringLiteral *SL = getStringLiteral(A1);
            if (SL)
            {
                StringRef Fmt = SL->getString();

                // fprintf(fp, "c") -> fputc('c', fp)
                if (N == 2 && Fmt.size() == 1 && !Fmt.contains('%'))
                {
                    if (CallExpr *FPutC = createCall("fputc", {createChar(Fmt[0]), A0}))
                    {
                        replaceCurrentStmt(FPutC);
                        return true;
                    }
                }

                // fprintf(fp, "str") -> fwrite("str", 1, len, fp)
                if (N == 2 && !Fmt.contains('%'))
                {
                    if (CallExpr *FWrite = createCall("fwrite", {A1, createInt(1), createInt(Fmt.size()), A0}))
                    {
                        replaceCurrentStmt(FWrite);
                        return true;
                    }
                }

                // fprintf(fp, "%s", str) -> fputs(str, fp)
                if (N == 3 && Fmt == "%s")
                {
                    if (CallExpr *FPuts = createCall("fputs", {A2, A0}))
                    {
                        replaceCurrentStmt(FPuts);
                        return true;
                    }
                }

                // fprintf(fp, "%c", ch) -> fputc(ch, fp)
                if (N == 3 && Fmt == "%c")
                {
                    if (CallExpr *FPutC = createCall("fputc", {A2, A0}))
                    {
                        replaceCurrentStmt(FPutC);
                        return true;
                    }
                }
            }
        }

        if (Name == "fputs" && N == 2)
        {
            if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(A1->IgnoreParenImpCasts()))
            {
                if (DRE->getDecl()->getName() == "stdout")
                {
                    const StringLiteral *SL = getStringLiteral(A0);

                    // fputs("str\n", stdout) -> puts("str")
                    if (SL && SL->getString().ends_with("\n"))
                    {
                        if (CallExpr *Puts = createCall("puts", {createString(SL->getString().drop_back(1))}))
                        {
                            replaceCurrentStmt(Puts);
                            return true;
                        }
                    }
                }
            }
        }

        // fabs(fabs(x)) -> fabs(x)
        if ((Name == "fabs" || Name == "fabsf" || Name == "fabsl") && N == 1)
        {
            if (CallExpr *Sub = dyn_cast<CallExpr>(A0->IgnoreParenImpCasts()))
            {
                if (FunctionDecl *SubFD = Sub->getDirectCallee())
                {
                    if (SubFD->getName() == Name)
                    {
                        replaceCurrentStmt(Sub);
                        return true;
                    }
                }
            }
        }

        if ((Name == "ceil" || Name == "ceilf" || Name == "ceill" ||
             Name == "floor" || Name == "floorf" || Name == "floorl" ||
             Name == "trunc" || Name == "truncf" || Name == "truncl" ||
             Name == "round" || Name == "roundf" || Name == "roundl") &&
            N == 1)
        {
            // ceil((int)x) -> (int)x
            if (isCastFromInteger(A0))
            {
                replaceCurrentStmt(A0);
                return true;
            }
        }

        if ((Name == "pow" || Name == "powf" || Name == "powl") && N == 2)
        {
            double Exp;
            if (getFloatValue(A1, Exp))
            {
                // pow(x, 0.0) -> 1.0
                if (Exp == 0.0)
                {
                    replaceCurrentStmt(createFloat(1.0));
                    return true;
                }

                // pow(x, 1.0) -> x
                if (Exp == 1.0)
                {
                    replaceCurrentStmt(A0);
                    return true;
                }
            }
        }

        if (Name == "isdigit" && N == 1)
        {
            // isdigit(x) -> (unsigned)x - '0' < 10
            Expr *Cast = createCast(Ctx.UnsignedIntTy, A0);
            Expr *Sub = BinaryOperator::Create(
                Ctx, Cast, createChar('0'), BO_Sub, Ctx.UnsignedIntTy,
                VK_PRValue, OK_Ordinary, SourceLocation(), FPOptionsOverride());
            Expr *Comp = BinaryOperator::Create(
                Ctx, Sub, createInt(10), BO_LT, Ctx.IntTy, VK_PRValue,
                OK_Ordinary, SourceLocation(), FPOptionsOverride());
            replaceCurrentStmt(Comp);
            return true;
        }

        if (Name == "isascii" && N == 1)
        {
            // isascii(x) -> (unsigned)x < 128
            Expr *Cast = createCast(Ctx.UnsignedIntTy, A0);
            Expr *Comp = BinaryOperator::Create(
                Ctx, Cast, createInt(128), BO_LT, Ctx.IntTy, VK_PRValue,
                OK_Ordinary, SourceLocation(), FPOptionsOverride());
            replaceCurrentStmt(Comp);
            return true;
        }

        // realloc(0, size) -> malloc(size)
        if (Name == "realloc" && N == 2 && isZero(A0))
        {
            if (CallExpr *Malloc = createCall("malloc", {A1}))
            {
                replaceCurrentStmt(Malloc);
                return true;
            }
        }

        return true;
    }

private:
    ASTContext &Ctx;
    std::vector<DynTypedNode> Ancestors;

    const StringLiteral *getStringLiteral(const Expr *E)
    {
        return dyn_cast<StringLiteral>(E->IgnoreParenImpCasts());
    }

    bool getFloatValue(const Expr *E, double &Val)
    {
        llvm::APFloat F(0.0);
        if (!E->EvaluateAsFloat(F, Ctx))
            return false;

        Val = F.convertToDouble();
        return true;
    }

    bool getIntValue(const Expr *E, llvm::APSInt &Val)
    {
        Expr::EvalResult Result;
        if (!E->EvaluateAsInt(Result, Ctx))
            return false;

        Val = Result.Val.getInt();
        return true;
    }

    bool matchesValue(const Expr *E, double Target)
    {
        llvm::APSInt IVal;
        if (getIntValue(E, IVal))
            return IVal == Target;

        double FVal;
        if (getFloatValue(E, FVal))
            return FVal == Target;

        return false;
    }

    bool isZero(const Expr *E)
    {
        llvm::APSInt I;
        if (getIntValue(E, I))
            return I == 0;

        double F;
        if (getFloatValue(E, F))
            return F == 0.0;

        if (E->IgnoreParenImpCasts()->isNullPointerConstant(Ctx, Expr::NPC_ValueDependentIsNull))
            return true;

        return false;
    }

    bool isCastFromInteger(const Expr *E)
    {
        if (const CastExpr *CE = dyn_cast<CastExpr>(E->IgnoreParens()))
            return CE->getSubExpr()->getType()->isIntegerType();

        return false;
    }

    bool isSameExpr(const Expr *A, const Expr *B)
    {
        if (const DeclRefExpr *DRA = dyn_cast<DeclRefExpr>(A->IgnoreParenCasts()))
        {
            if (const DeclRefExpr *DRB = dyn_cast<DeclRefExpr>(B->IgnoreParenCasts()))
                return DRA->getDecl() == DRB->getDecl();
        }

        return false;
    }

    CallExpr *createCall(StringRef FuncName, ArrayRef<Expr *> Args)
    {
        IdentifierInfo &II = Ctx.Idents.get(FuncName);
        FunctionDecl *FD = nullptr;
        for (NamedDecl *ND : Ctx.getTranslationUnitDecl()->lookup(&II))
        {
            if ((FD = dyn_cast<FunctionDecl>(ND)))
                break;
        }

        if (!FD)
            return nullptr;

        QualType FnType = FD->getType();
        DeclRefExpr *DRE = DeclRefExpr::Create(
            Ctx, NestedNameSpecifierLoc(), SourceLocation(), FD, false,
            SourceLocation(), FnType, VK_LValue);
        Expr *Callee = ImplicitCastExpr::Create(
            Ctx, Ctx.getPointerType(FnType), CK_FunctionToPointerDecay, DRE,
            nullptr, VK_PRValue, FPOptionsOverride());

        return CallExpr::Create(
            Ctx, Callee, Args, FD->getCallResultType(), VK_PRValue,
            SourceLocation(), FPOptionsOverride());
    }

    IntegerLiteral *createInt(int64_t Val)
    {
        return IntegerLiteral::Create(Ctx, llvm::APInt(64, Val, true), Ctx.IntTy, SourceLocation());
    }

    CharacterLiteral *createChar(char Val)
    {
        return new (Ctx) CharacterLiteral(static_cast<unsigned>(Val), CharacterLiteralKind::Ascii, Ctx.IntTy, SourceLocation());
    }

    FloatingLiteral *createFloat(double Val)
    {
        return FloatingLiteral::Create(Ctx, llvm::APFloat(Val), true, Ctx.DoubleTy, SourceLocation());
    }

    StringLiteral *createString(StringRef Str)
    {
        QualType StrTy = Ctx.getConstantArrayType(
            Ctx.CharTy, llvm::APInt(32, Str.size() + 1), nullptr,
            ArraySizeModifier::Normal, 0);

        return StringLiteral::Create(
            Ctx, Str, StringLiteralKind::Ordinary,
            false, StrTy, SourceLocation());
    }

    Expr *createCast(QualType Type, Expr *E)
    {
        TypeSourceInfo *TInfo = Ctx.getTrivialTypeSourceInfo(Type);
        return CStyleCastExpr::Create(
            Ctx, Type, VK_PRValue, CK_IntegralCast, E, nullptr,
            FPOptionsOverride(), TInfo, SourceLocation(), SourceLocation());
    }

    Expr *createUnaryNeg(Expr *E)
    {
        return UnaryOperator::Create(
            Ctx, E, UO_Minus, E->getType(), VK_PRValue, OK_Ordinary,
            SourceLocation(), false, FPOptionsOverride());
    }

    Expr *createDeref(Expr *E)
    {
        return UnaryOperator::Create(
            Ctx, E, UO_Deref, Ctx.CharTy, VK_LValue, OK_Ordinary,
            SourceLocation(), false, FPOptionsOverride());
    }

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
    llvm::cl::ParseCommandLineOptions(argc, argv, "expression simplifier\n");
    FixedCompilationDatabase Compilations(".", {"-Wno-everything", "-Wno-error", "-w"});
    ClangTool Tool(Compilations, {InputFile});
    return Tool.run(newFrontendActionFactory<Action>().get());
}
