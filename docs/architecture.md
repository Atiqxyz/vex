# VEX compiler architecture

The current development build implements source management, diagnostics, lexing, parsing, an AST, semantic analysis with a reusable typed model, verified SSA IR lowering, a portable C11 source backend, and a textual LLVM IR backend for the currently supported language subset.

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

The backend boundary is represented by `Backend`, `BackendResult`, and `create_backend()`. A backend consumes verified IR and returns its generated output together with structured diagnostics. The factory registers C and LLVM backends. `emit-c` and `emit-llvm` select the corresponding emitter; `build`/`run` use the LLVM native-build path with the bundled Zig/LLVM toolchain when available (or an installed Clang driver), while `build-c`/`run-c` select the optional C11 backend. The LLVM emitter writes textual LLVM IR using opaque pointers; native executable generation is delegated to the selected toolchain.

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

The AST represents function and structure declarations, bindings, returns, blocks, conditionals, counted and condition-driven loops, loop exits/continues, expression statements, literals, names, unary/binary operations, assignments with their original assignment operator, calls, grouping, explicit multiline expressions, structure literals and member access, array literals and indexing, and integer ranges.

### Semantic analysis

The semantic checker is a separate pass over the AST. It first registers top-level function signatures, allowing forward function calls, then checks statements and expressions in lexical scopes.

The type model covers primitive `int`, unsigned 8-bit `byte`, `float`, `bool`, `string`, and `void` values; nominal structures; one-dimensional fixed arrays; and recursively nested dynamic/mixed fixed-dynamic jagged arrays. Structure declarations are checked for unique fields and valid field types, including earlier-declared structures and fixed/dynamic nested arrays of primitives or structures. Direct recursive by-value layouts are rejected, while recursive dynamic-array fields are supported. Fixed dimensions and structures use value semantics; every dynamic dimension is a shared header reference, including when nested inside copied structures or fixed arrays. Local bindings can infer their type from an initializer. Semantic analysis checks compound assignment on named bindings and nested field/index paths, exact-once target-index evaluation during lowering, complete recursive array shapes, mutability, `length`, `capacity`, `reserve`, `push`, `pop`, string concatenation and joining, arithmetic and logical operators, control flow, function signatures, returns, duplicate declarations, and undefined names. Parsed import declarations currently receive a semantic error because module resolution is not implemented. Binary built-ins validate `byte[]` operands, integer offsets/values, and immutable-buffer mutation; file reads require a mutable destination buffer.

The current numeric conversion rule permits widening from `int` to `float`; narrowing is rejected. Integer remainder uses truncating signed semantics in both backends: a non-zero result has the dividend's sign, division by zero traps, and `INT64_MIN % -1` returns zero without executing an overflowing remainder instruction. Nested blocks have lexical scopes and can shadow outer bindings. `const` bindings cannot be assigned after declaration.

The `say(value)` intrinsic and the first mathematical standard-library functions (`sqrt`, `sin`, `cos`, `pow`, and `abs`) are typed, verified, and emitted by both backends. Fixed one-dimensional arrays and dynamic/mixed fixed-dynamic jagged arrays support primitive or structure leaves, checked recursive indexed reads and writes, nested `for-in` iteration, `length`, `capacity`, `reserve`, `push`, and `pop`. Fixed dimensions are inline value aggregates; dynamic dimensions are shared heap-backed references with geometric growth, explicit reservation, runtime bounds checks, alias-preserving header updates, and program-exit cleanup. All-fixed nested aggregates remain deferred pending a target-aware size and stack policy. Strings are length-aware byte sequences with byte length, equality, `+`, `+=`, and `join(string[])`; concatenation and joining preserve embedded NUL bytes and use tracked allocations. Named structures support nested structure and nested-array fields, arrays of structures, field access and mutation, value-copy behavior, function parameters/returns, and recursive dynamic-array fields. Both C and LLVM backends lower these forms. Contiguous rectangular multidimensional arrays, slices, user-visible pointers, modules, generic types, and function values remain future work.

### Typed semantic model and SSA IR

Semantic analysis returns a `SemanticModel` alongside diagnostics. It records expression types, resolved variable and assignment bindings, parameter and local binding identities, resolved direct-call names, and function signatures. The model is tied to the lifetime of the AST it describes.

The IR is backend-independent and uses SSA values. Functions contain basic blocks with explicit terminators. Block parameters receive values from incoming branches and serve as control-flow merge points. Array values carry a scalar or nominal structure leaf type plus the complete outermost-first dimension vector; indexing derives the immediate element type by removing one dimension. Fixed dimensions are typed aggregate values, while dynamic dimensions are shared runtime-header pointers. The independent verifier compares complete nested shapes for construction, indexing, mutation, collection operations, structure fields, function calls, and returns. All-fixed nested source aggregates remain gated by semantic analysis until a target-aware size/stack policy is enforced. The [nested-array design proposal](nested-array-design.md) records that remaining gate. String concatenation/joining and byte length, checked byte conversions and bitwise operations, owning byte slices, endian-aware binary integer access, whole-file binary I/O, dynamic-array length/capacity/reservation, and collection mutation have dedicated typed operations. Source spans are retained for diagnostics.

The AST-to-IR lowerer consumes the semantic model. It preserves assignment operators in the AST and lowers compound assignment by resolving the target path once, reading the old leaf before evaluating the right-hand side, applying the typed operation, and rebuilding the aggregate to its named root. It also lowers string concatenation/joining, byte slicing and binary/file operations, dynamic-array length/capacity/reservation and mutation, fixed-array literals and indexing, direct calls, control flow, and a synthesized entry function when top-level statements are used without an explicit `main()`. Integer-to-float widening becomes an explicit conversion instruction.

The independent verifier checks instruction and call types, SSA definitions and uses, branch targets and block arguments, terminators, reachability, and dominance. The `vex ir` CLI command runs frontend validation, lowering, verification, and IR printing. It does not execute programs.

### Backend interface and C source backend

The backend interface consumes verified IR and keeps code generation behind a replaceable boundary. `BackendResult` carries generated output and diagnostics; backend-specific details remain inside the implementation. The C backend is a registered implementation and continues to expose its existing `CBackend::generate()` API for C-specific callers. Both C and LLVM backends implement the current binary-buffer and whole-file I/O IR operations.

The C backend consumes only verified IR. It emits C11 source without mixing backend logic into parsing or semantic analysis. Fixed arrays use recursively generated C struct wrappers, including nested fixed dimensions and function return types; dynamic dimensions use the tracked runtime header pointer. Element storage uses the C compiler's `sizeof` for the complete immediate element type, and a shared runtime bounds check protects indexed reads and writes.

- Values use fixed-width C types where available. VEX `byte` maps to `uint8_t`; strings use a pointer-plus-length representation, preserving embedded NUL bytes. Non-empty concatenation and joining use one tracked allocation containing both the lifetime node and byte payload; empty concatenation returns the other descriptor and empty joining returns an empty descriptor without allocating. Dynamic arrays use a tracked header and contiguous element buffer; `capacity` reports the allocated slots and `reserve` reallocates only when the requested absolute minimum exceeds the current capacity.
- Function symbols are mangled into a private generated namespace. All prototypes precede definitions, supporting forward calls.
- Basic-block parameters are represented by local variables. Each control-flow edge uses temporaries to implement parallel copies before a `goto`, so loop-carried values and branch merges are not corrupted by sequential assignment.
- Integer arithmetic uses checked helper functions to avoid C signed-overflow undefined behavior. Runtime failure is reported for overflow and invalid integer division/remainder.
- The output helpers print the current primitive types. The generated `main` adapter accepts a zero-argument VEX `main()` returning `void` or `int`.

The `vex emit-c` command runs lexing, parsing, semantic analysis, IR lowering, verification, and C emission. It writes C11 source to standard output; it does not invoke the C compiler or produce an executable.

### LLVM IR backend

The LLVM backend translates verified VEX IR into textual LLVM IR. It maps VEX `int`, `byte`, `float`, `bool`, and `string` values to `i64`, `i8`, `double`, `i1`, and a pointer-plus-length aggregate. Fixed dimensions map recursively to LLVM array aggregates; dynamic dimensions use a shared runtime header and target-data-layout-derived element strides for the full immediate element type, including nested fixed aggregates. The backend emits checked indexing, geometric growth, capacity queries, reservation with overflow-checked byte sizing, byte-preserving string concatenation/joining, byte-slice copies, endian-aware binary codecs, whole-file binary I/O, and program-entry cleanup. Control-flow edges use explicit edge blocks to transfer VEX block parameters, and checked integer operations use LLVM overflow intrinsics and trap paths.

`vex emit-llvm` prints generated LLVM IR. `vex build-llvm` and `vex run-llvm` use a Clang-compatible driver to optimize and generate a host executable. Release packages bundle a pinned Zig toolchain that supplies the Clang/LLVM driver; development builds can also use `VEX_LLVM_CC`, `LLVM_CC`, or a discoverable Clang installation. Optimization is initially delegated to the selected toolchain's `-O2` pipeline. The C backend remains available as a fallback.

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
- [x] Inferred fixed-size primitive arrays, indexing, indexed updates, and array iteration
- [x] Half-open and inclusive integer ranges in `for-in` loops
- [ ] Dynamic collections, slices, and general iterable types
- [x] Typed SSA intermediate representation, semantic metadata, verifier, and AST-to-IR lowering
- [x] Backend interface and factory with C and LLVM backends registered
- [x] Initial C11 code-generation backend
- [x] Initial textual LLVM IR code-generation backend
- [x] LLVM-backed native build/run path through external Clang
- [ ] Bundle/provision LLVM and Clang components for standalone installation
- [ ] Runtime and standard library expansion
- [ ] Native machine-code backend
- [ ] Language server and editor tooling
