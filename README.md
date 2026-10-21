# c-standardization-tools
Clang-based tools for source code standardization

## Overview

Standardization is useful when a consistent textual form matters, for example when preparing datasets of source code for training machine learning models. These tools collapse superficial variation in C code into a canonical form that stays close to what a compiler actually emits, in which literal types are directly recoverable from syntax, without semantic analysis.

| Tool | Description |
| --- | --- |
| [`macro-expand`](macro-expand.cpp) | Expands macros, removes comments, cleans up whitespace, keeps system header includes. |
| [`expr-simplify`](expr-simplify.cpp) | Applies arithmetic identities, folds supported math calls, simplifies selected library calls. |
| [`binop-reorder`](binop-reorder.cpp) | Moves constant operands to the left of addition, subtraction, and multiplication. |
| [`unop-simplify`](unop-simplify.cpp) | Simplifies negation into subtraction where possible. |
| [`type-simplify`](type-simplify.cpp) | Normalizes types, removes trivial typedefs, evaluates constants, standardizes literals. |
| [`implicit-cast-reveal`](implicit-cast-reveal.cpp) | Inserts explicit C-style casts for implicit conversions. |
| [`size-check`](size-check.cpp) | Returns `1` if an initialized variable exceeds 256 bytes. |

## Usage

Build all executables in the repository root:

```sh
make
```

Every tool takes exactly one input file as a positional argument. The tools can be chained together to form a complete standardization pipeline:

```sh
./macro-expand input.c > step1.c
./expr-simplify step1.c > step2.c
./binop-reorder step2.c > step3.c
./unop-simplify step3.c > step4.c
./type-simplify step4.c > output.c
```
