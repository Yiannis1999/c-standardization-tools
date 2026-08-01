# c-standardization-tools

Clang/LLVM-based command-line tools that normalize and simplify C source code.

## Overview

This repository provides small single-purpose tools that parse a C source file with Clang and print transformed output (or a status check result).  
They are designed to support C code standardization pipelines.

Included tools:

- `macro-expand` – expands macros and prints a tokenized translation unit.
- `expr-simplify` – simplifies expressions and common library calls.
- `binop-reorder` – normalizes arithmetic binary operators (for example, moves constants to the left for `+`/`*` and rewrites subtraction forms).
- `unop-simplify` – simplifies unary-minus related patterns.
- `type-simplify` – canonicalizes arithmetic types and literals, resolves some inferred type forms, and removes simple typedef aliases.
- `implicit-cast-reveal` – rewrites implicit casts into explicit C-style casts.
- `size-check` – returns a failing status if a variable with initializer exceeds 256 bytes.

## Requirements

- C++ compiler (`g++` in the default `Makefile`)
- LLVM + Clang development libraries
- `llvm-config` (or `llvm-config-18`)

## Build

From the repository root:

```bash
make
```

This builds all binaries listed above.

To remove built binaries:

```bash
make clean
```

## Usage

Each tool accepts one positional source file argument:

```bash
./<tool-name> <path-to-c-file>
```

Examples:

```bash
./expr-simplify input.c > output.c
./type-simplify input.c > output.c
./implicit-cast-reveal input.c > output.c
```

`size-check` is intended for validation in scripts/CI:

```bash
./size-check input.c
echo $?   # 0 = pass, 1 = large initialized object detected, other nonzero = tool failure
```

## Notes

- Most tools print rewritten source to standard output; redirect to files as needed.
- Tools use a fixed compilation database with warning suppression flags, so no external `compile_commands.json` is required for basic usage.
