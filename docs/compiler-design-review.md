# Compiler design review: lessons for VEX

**Reviewed:** 2026-11-12

VEX is a small language project, so the goal is not to reproduce Clang or GCC. The goal is to borrow the architectural boundaries that have proven useful while avoiding features VEX does not need.

## 1. Clang: separate the compiler into reusable layers

Clang documents distinct libraries for basic source/diagnostic facilities, lexing, parsing, semantic analysis, ASTs, and code generation. Its parser can be kept separate from a particular AST implementation, while semantic analysis supplies the language-specific meaning of parsed constructs.

**What VEX adopts:**

- Source buffers and source locations are a foundational layer, not ad-hoc line counters inside the lexer.
- Tokens refer to source locations/spans instead of owning a duplicate copy of every spelling.
- Diagnostics have a severity, a stable identifier, and a source range.
- Lexing, parsing, semantic analysis, and code generation are separate compiler stages.

**What VEX does not copy:**

- Clang's lexer and parser support the C family, preprocessing, macro expansion, source rewriting, and C++ language complexities. VEX does not need a C preprocessor or C++ template machinery merely because it is implemented in C++.
- The VEX frontend can remain much smaller and use a straightforward token stream and AST.

References:
- [Clang library architecture](https://clang.llvm.org/features.html)
- [Clang internals manual: diagnostics](https://clang.llvm.org/docs/InternalsManual.html)
- [Clang Token interface](https://github.com/llvm/llvm-project/blob/main/clang/include/clang/Lex/Token.h)
- [Clang SourceLocation interface](https://github.com/llvm/llvm-project/blob/main/clang/include/clang/Basic/SourceLocation.h)

## 2. GCC: do not bind the frontend directly to machine code

GCC separates frontend representations from later optimization and target lowering. Its internals describe GENERIC as a language-independent representation, GIMPLE as a more restricted form used for many optimizations, and RTL as a lower-level representation. GCC also uses static single assignment (SSA) in much of its optimization infrastructure.

**What VEX adopts:**

- Plan for a backend-independent IR between semantic analysis and code generation.
- Keep source-level AST information for diagnostics and developer tools.
- Introduce optimization only after the language semantics and typed IR are stable.
- Treat C code generation as a practical bootstrap backend, not as the compiler's permanent internal representation.

**What VEX does not copy:**

- GCC's multiple IR layers and target machinery are far beyond what the current lexer prototype needs. VEX should introduce an IR only when there is enough parsed and typed language to justify it.

References:
- [GCC internals: Tree SSA and intermediate representations](https://gcc.gnu.org/onlinedocs/gccint/Tree-SSA.html)
- [GCC internals: overview](https://gcc.gnu.org/onlinedocs/gccint/Overview.html)

## 3. What this means for the current implementation

| Concern | Earlier prototype | Rebuilt direction |
|---|---|---|
| Source identity | Line and column copied into each token | Immutable `SourceFile` plus byte spans |
| Token spelling | Allocated `std::string` per token | Spelling is a view into the source buffer |
| Diagnostics | Message and start position only | Severity, stable code, message, source span |
| Keyword lookup | Runtime hash map and temporary string | `string_view` lookup over a fixed keyword table |
| Newlines in comments | Special-case behavior | Explicitly preserved as tokens |
| Tests | One lexer test executable | Separate source and lexer tests with checks that remain active in Release |
| CI | CMake commands repeated in workflow | Consistent CMake configure/build/test commands |

## 4. Design decisions to keep explicit

- Source offsets are byte-based. User-facing display uses one-based line and column values.
- Identifiers are ASCII-only in the current lexical design. Unicode identifiers would require a language-level normalization and classification policy.
- Token spelling remains in the source buffer; later stages must preserve the `SourceFile` lifetime or move to an owned source manager.
- Diagnostics use stable codes so command-line and editor frontends can render the same underlying data.
- The current stage still does not parse expressions, infer types, allocate memory, emit C, or generate native executables.

These decisions establish a maintainable frontend without pretending that VEX is already a complete compiler.
