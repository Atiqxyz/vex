# VEX compiler architecture

This document describes the intended compiler pipeline. Some stages are planned and are not implemented in the current development build.

## Pipeline

1. **Source loading** reads UTF-8 `.vex` source files and preserves source locations.
2. **Lexing** converts characters into tokens. This is the first implemented frontend stage.
3. **Parsing** will convert tokens into an abstract syntax tree (AST).
4. **Semantic analysis** will resolve names, scopes, type inference, and type errors.
5. **Intermediate representation (IR)** will provide a backend-independent representation for later optimization and code generation.
6. **Code generation** will initially target C as a practical route to native executables.
7. **Native toolchain** compilation will use an installed C compiler such as Clang or GCC. The Windows toolchain will be selected and tested separately.

## Implementation language

The first VEX compiler is implemented in C++20. This does not constrain the implementation language of future compiler versions. A self-hosting compiler is a long-term goal, not a current capability.

## Frontend design principles

- Keep source locations on tokens and diagnostics.
- Keep the lexer and parser independent of the eventual code-generation backend.
- Preserve newline tokens because newlines separate statements in VEX.
- Treat built-in operations such as `say(...)` as ordinary calls supplied by the language environment, not as special lexer keywords.
- Test each compiler stage independently.

## Current implementation status

- [x] Token model and source locations
- [x] Lexer for the initial token set
- [x] Lexer diagnostics
- [x] Lexer unit tests
- [ ] Parser and AST
- [ ] Name resolution and type checking
- [ ] Intermediate representation
- [ ] C code generation
- [ ] Native executable generation
- [ ] Language server and editor tooling
