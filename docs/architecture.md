# VEX compiler architecture

The current development build implements source management, diagnostics, lexing, parsing, an initial AST, and a first semantic-analysis pass.

## Intended pipeline

~~~text
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
Semantic analysis ------> scopes, resolved names, inferred types, diagnostics
    |
    v
Typed intermediate representation
    |
    +--------------------> C bootstrap backend (candidate)
    |
    +--------------------> native/LLVM backend (future option)
~~~

The frontend should not depend on a particular output backend. In particular, the parser should not emit C text directly. A typed, backend-independent representation gives VEX room to change its native code-generation strategy without rewriting the language frontend.

## Implemented layers

### Source management

`SourceFile` owns an immutable source buffer and its path. A `SourceSpan` stores a half-open byte range into that buffer. Line starts are indexed once, allowing token and diagnostic locations to be recovered without copying token spelling.

### Diagnostics

Diagnostics carry a severity, stable code, message, and source span. The command-line renderer displays the source line and a caret. Lexer, parser, and semantic diagnostics share the same model.

### Lexing

The lexer converts characters into token kinds and source spans. It preserves newline tokens because newlines separate VEX statements, including newlines inside block comments. Identifier recognition and keyword lookup are separate operations.

### Parsing and AST

The parser uses recursive descent for declarations and statements, plus precedence climbing for expressions. This follows a practical pattern used in LLVM's Kaleidoscope frontend tutorial, while keeping the grammar and AST specific to VEX.

AST nodes retain source spans instead of copying every identifier or literal spelling. The source file must therefore outlive the AST when source text is needed for diagnostics or AST printing.

The initial AST represents function declarations, bindings, returns, blocks, conditionals, loops, expression statements, literals, names, unary/binary operations, assignments, calls, grouping, and explicit multiline expressions.

### Semantic analysis

The semantic checker is a separate pass over the AST. It first registers top-level function signatures, allowing forward function calls, then checks statements and expressions in lexical scopes.

The initial type model covers `int`, `float`, `bool`, `string`, and `void` returns. Local bindings can infer their type from an initializer. Explicit annotations, assignments, arithmetic and logical operators, conditions, repeat counts, function argument counts and types, return statements, duplicate declarations, and undefined names are checked.

The current numeric conversion rule permits widening from `int` to `float`; narrowing is rejected. Nested blocks have lexical scopes and can shadow outer bindings. `const` bindings cannot be assigned after declaration.

The `say(value)` intrinsic is registered for semantic checking but has no runtime implementation. The parser accepts `for-in`, but semantic analysis reports that collection types are not implemented yet. Arrays, structures, pointers, modules, generic types, function values, and code generation remain future work.

## Design constraints

- Use byte offsets internally; convert them to one-based line and column values for display.
- Keep tokens small and avoid allocating a separate string for every token.
- Keep diagnostics independent of terminal formatting so editor/LSP clients can reuse them.
- Keep syntax parsing, semantic analysis, and code generation in separate components.
- Add tests for each layer, including malformed input and release builds.
- Keep language design decisions in `SPEC.md`; do not silently turn provisional examples into language guarantees.

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
- [x] Lexical scopes and nested shadowing
- [x] Top-level function signature collection and forward calls
- [x] Identifier resolution and duplicate declaration diagnostics
- [x] Local primitive type inference and annotation checking
- [x] Operator, condition, assignment, call, and return type checking
- [x] Command-line semantic-check command
- [x] Cross-platform CMake build and CI configuration
- [ ] Collections and iterable type checking
- [ ] Typed intermediate representation
- [ ] Code-generation backend
- [ ] Runtime and standard library
- [ ] Language server and editor tooling
