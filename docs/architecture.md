# VEX compiler architecture

This document describes the compiler boundaries VEX is being built around. The current development build implements source management, diagnostics, lexing, parsing, and an initial AST.

## Intended pipeline

\`\`\`text
Source files
    |
    v
Source management ------> source spans and line/column mapping
    |
    v
Lexer ------------------> tokens + lexical diagnostics
    |
    v
Parser -----------------> AST + syntax diagnostics
    |
    v
Semantic analysis ------> resolved names, scopes, types, diagnostics
    |
    v
Typed intermediate representation
    |
    +--------------------> C bootstrap backend (candidate)
    |
    +--------------------> native/LLVM backend (future option)
\`\`\`

The frontend should not depend on a particular output backend. In particular, the parser should not emit C text directly. A typed, backend-independent representation gives VEX room to change its native code-generation strategy without rewriting the language frontend.

## Implemented layers

### Source management

\`SourceFile\` owns an immutable source buffer and its path. A \`SourceSpan\` stores a half-open byte range into that buffer. Line starts are indexed once, allowing token and diagnostic locations to be recovered without copying token spelling.

### Diagnostics

Diagnostics carry a severity, stable code, message, and source span. The command-line renderer displays the source line and a caret. Lexer and parser diagnostics share the same model.

### Lexing

The lexer converts characters into token kinds and source spans. It preserves newline tokens because newlines separate VEX statements, including newlines inside block comments. Identifier recognition and keyword lookup are separate operations.

### Parsing and AST

The parser uses recursive descent for declarations and statements, plus precedence climbing for expressions. This follows a practical pattern used in LLVM's Kaleidoscope frontend tutorial, while keeping the grammar and AST specific to VEX.

AST nodes retain source spans instead of copying every identifier or literal spelling. The source file must therefore outlive the AST when source text is needed for diagnostics or AST printing.

The initial AST represents function declarations, bindings, returns, blocks, conditionals, loops, expression statements, literals, names, unary/binary operations, assignments, calls, grouping, and explicit multiline expressions.

The parser reports syntax diagnostics and attempts to recover at statement boundaries. It does not resolve names, infer types, validate function signatures, execute code, or generate machine code.

## Design constraints

- Use byte offsets internally; convert them to one-based line and column values for display.
- Keep tokens small and avoid allocating a separate string for every token.
- Keep diagnostics independent of terminal formatting so editor/LSP clients can reuse them.
- Keep syntax parsing, semantic analysis, and code generation in separate components.
- Add tests for each layer, including malformed input and release builds.
- Keep language design decisions in \`SPEC.md\`; do not silently turn provisional examples into language guarantees.

## Reference points

The architecture borrows broad design lessons from established compilers, not their implementation complexity. See [the compiler design review](compiler-design-review.md) for the specific comparisons.

## Current implementation status

- [x] Immutable source buffers and source spans
- [x] Line/column mapping for LF, CRLF, and CR
- [x] Structured diagnostics with source excerpts
- [x] Lexer and stable token kinds
- [x] Lexer and source unit tests
- [x] Recursive-descent statement parser
- [x] Precedence-aware expression parser
- [x] Initial AST and AST printer
- [x] Parser syntax diagnostics and recovery
- [x] Cross-platform CMake build and CI configuration
- [ ] Name resolution and type checking
- [ ] Typed intermediate representation
- [ ] Code-generation backend
- [ ] Runtime and standard library
- [ ] Language server and editor tooling
