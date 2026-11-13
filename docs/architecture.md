# VEX compiler architecture

The current development build implements source management, diagnostics, lexing, parsing, an initial AST, semantic analysis with a reusable typed model, verified SSA IR lowering, and an initial C source backend for the currently supported language subset.

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
    |                     + typed semantic model
    v
IR lowering ------------> typed SSA values, CFG, source spans
    |
    v
IR verifier ------------> verified module or IR diagnostics
    |
    v
Backend interface ------> backend-neutral output + diagnostics
    |
    +-------------------> C backend -> portable C11 source (implemented)
    |
    +-------------------> LLVM backend -> textual LLVM IR (initial implementation)
                          -> Clang native executable path (initial integration)
~~~

The frontend should not depend on a particular output backend. In particular, the parser should not emit C text directly. A typed, backend-independent representation gives VEX room to change its native code-generation strategy without rewriting the language frontend.

The backend boundary is represented by `Backend`, `BackendResult`, and `create_backend()`. A backend consumes verified IR and returns its generated output together with structured diagnostics. The factory registers C and LLVM backends. `emit-c` and `emit-llvm` select the corresponding emitter; `build`/`run` use the C bootstrap route, while `build-llvm`/`run-llvm` send generated LLVM IR to an installed Clang driver. The LLVM emitter writes textual LLVM IR using opaque pointers; the compiler does not link LLVM libraries or bundle an LLVM toolchain yet.

## Implemented layers

### Source management

`SourceFile` owns an immutable source buffer and its path. A `SourceSpan` stores a half-open byte range into that source buffer. Line starts are indexed once, allowing token and diagnostic locations to be recovered without copying token spelling.

### Diagnostics

Diagnostics carry a severity, stable code, message, and source span. The command-line renderer displays the source line and a caret. Lexer, parser, and semantic diagnostics share the same model.

### Lexing

The lexer converts characters into token kinds and source spans. It preserves newline tokens because newlines separate VEX statements, including newlines inside block comments. Identifier recognition and keyword lookup are separate operations.

### Parsing and AST

The parser uses recursive descent for declarations and statements, plus precedence climbing for expressions. This follows a practical pattern used in LLVM's Kaleidoscope frontend tutorial, while keeping the grammar and AST specific to VEX.

AST nodes retain source spans instead of copying every identifier or literal spelling. The source file must therefore outlive the AST when source text is needed for diagnostics or AST printing.

The AST represents function declarations, bindings, returns, blocks, conditionals, counted and condition-driven loops, loop exits/continues, expression statements, literals, names, unary/binary operations, assignments, calls, grouping, and explicit multiline expressions.

### Semantic analysis

The semantic checker is a separate pass over the AST. It first registers top-level function signatures, allowing forward function calls, then checks statements and expressions in lexical scopes.

The initial type model covers `int`, `float`, `bool`, `string`, and `void` returns. Local bindings can infer their type from an initializer. Explicit annotations, assignments (including compound assignments), arithmetic and logical operators, conditions, repeat counts, while conditions, loop-control placement, function argument counts and types, return statements, duplicate declarations, and undefined names are checked.

The current numeric conversion rule permits widening from `int` to `float`; narrowing is rejected. Nested blocks have lexical scopes and can shadow outer bindings. `const` bindings cannot be assigned after declaration.

The `say(value)` intrinsic is registered for semantic checking and is implemented in the initial generated-C runtime. The first mathematical standard-library functions (`sqrt`, `sin`, `cos`, `pow`, and `abs`) are registered with typed signatures, lowered to explicit math IR instructions, verified, and emitted as C math-library calls. The C bootstrap build links the platform math library where required. The parser accepts `for-in`, but semantic analysis reports that collection types are not implemented yet. Arrays, structures, pointers, modules, generic types, and function values remain future work.

### Typed semantic model and SSA IR

Semantic analysis returns a `SemanticModel` alongside diagnostics. It records expression types, resolved variable and assignment bindings, parameter and local binding identities, resolved direct-call names, and function signatures. The model is tied to the lifetime of the AST it describes.

The initial IR is backend-independent and uses SSA values. Functions contain basic blocks with explicit terminators. Block parameters receive values from incoming branches and serve as control-flow merge points. The IR keeps the supported primitive types explicit and retains source spans for diagnostics.

The AST-to-IR lowerer consumes the semantic model. It handles function parameters, constants, name references, assignments (compound assignments are desugared in the parser), primitive operators, direct calls, `say`, `if/else`, short-circuit `&&` and `||`, counted `repeat` loops, condition-driven `while` loops, SSA-safe `break`/`continue` edges, and a synthesized entry function when top-level statements are used without an explicit `main()`. Integer-to-float widening becomes an explicit conversion instruction. Constructs that are parsed but not semantically supported, such as `for-in`, are not lowered.

The independent verifier checks instruction and call types, SSA definitions and uses, branch targets and block arguments, terminators, reachability, and dominance. The `vex ir` CLI command runs frontend validation, lowering, verification, and IR printing. It does not execute programs.

### Backend interface and C source backend

The backend interface consumes verified IR and keeps code generation behind a replaceable boundary. `BackendResult` carries generated output and diagnostics; backend-specific details remain inside the implementation. The C backend is the first registered implementation and continues to expose its existing `CBackend::generate()` API for C-specific callers.

The initial C backend consumes only verified IR. It emits C11 source without mixing backend logic into parsing or semantic analysis.

- Values use fixed-width C types where available. VEX strings use a pointer-plus-length representation, preserving embedded NUL bytes.
- Function symbols are mangled into a private generated namespace. All prototypes precede definitions, supporting forward calls.
- Basic-block parameters are represented by local variables. Each control-flow edge uses temporaries to implement parallel copies before a `goto`, so loop-carried values and branch merges are not corrupted by sequential assignment.
- Integer arithmetic uses checked helper functions to avoid C signed-overflow undefined behavior. Runtime failure is reported for overflow and invalid integer division/remainder.
- The output helpers print the current primitive types. The generated `main` adapter accepts a zero-argument VEX `main()` returning `void` or `int`.

The `vex emit-c` command runs lexing, parsing, semantic analysis, IR lowering, verification, and C emission. It writes C11 source to standard output; it does not invoke the C compiler or produce an executable.

### Initial LLVM IR backend

The LLVM backend translates verified VEX IR into textual LLVM IR. It maps VEX `int`, `float`, `bool`, and `string` values to `i64`, `double`, `i1`, and a pointer-plus-length aggregate. Control-flow edges use explicit edge blocks to transfer VEX block parameters, and checked integer operations use LLVM overflow intrinsics and trap paths. The output runtime handles current primitive `say` values, and math intrinsics call the corresponding math symbols.

`vex emit-llvm` prints generated LLVM IR. `vex build-llvm` and `vex run-llvm` use an installed Clang driver to optimize and generate a host executable. This first native path is not yet a bundled toolchain: Clang must be installed and discoverable through `VEX_LLVM_CC` or `LLVM_CC`, or on PATH. Optimization is initially delegated to Clang's `-O2` pipeline. The C backend remains available as a fallback.

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
- [x] Boolean while-loop conditions and loop-control scope checking
- [x] While loops, break/continue, and compound assignment lowering
- [x] Implicit top-level entry function when no explicit main is declared
- [x] Command-line semantic-check and verified IR-inspection commands
- [x] Cross-platform CMake build and CI configuration
- [ ] Collections and iterable type checking
- [x] Typed SSA intermediate representation, semantic metadata, verifier, and AST-to-IR lowering
- [x] Backend interface and factory with C and LLVM backends registered
- [x] Initial C11 code-generation backend
- [x] Initial textual LLVM IR code-generation backend
- [x] LLVM-backed native build/run path through external Clang
- [ ] Bundle/provision LLVM and Clang components for standalone installation
- [ ] Runtime and standard library expansion
- [ ] Native machine-code backend
- [ ] Language server and editor tooling
