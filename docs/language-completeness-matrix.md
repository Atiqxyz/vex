# VEX language completeness matrix

**Purpose:** Track the capabilities a general-purpose systems and scientific-computing language needs, compare them against what VEX actually implements, and expose design gaps before choosing the next patch.

**Scope:** This is an audit and planning document, not a promise that every listed feature will be implemented. The current development branch is the reference point. \`SPEC.md\` remains authoritative for exact current syntax and semantics.

## Status key

- **Implemented** — present end to end in the current compiler/runtime path, with tests.
- **Partial** — some useful behavior exists, but important parts or edge cases are missing.
- **Not implemented** — not currently available as a VEX language capability.
- **Design undecided** — we have not selected precise semantics or syntax.
- **Verify** — documentation suggests or implies behavior, but source-level tests are needed before we claim it.
- **Deferred** — deliberately postponed; not a blocker for the current core.

Do not upgrade a status based on syntax alone. A feature counts as implemented only when parsing, semantic rules, IR, both supported backends where applicable, tests, and documentation agree.

## 1. Functions and callable values

| Capability | Status | Audit notes / questions |
|---|---|---|
| Function declarations | Implemented | \`make name(...)\` declarations. |
| Typed parameters | Implemented | Current parameters require explicit types. |
| Explicit return types | Implemented | \`-> type\`; omitted return type means \`void\`. |
| Void functions | Implemented | No-value return behavior exists. |
| Value-returning functions | Implemented | Return compatibility is checked. |
| Argument count/type checking | Implemented | Static semantic checks exist. |
| Forward function calls | Implemented | Top-level signatures are registered before bodies. |
| Direct function calls | Implemented | Direct calls lower through typed IR. |
| Recursion | Implemented | Direct recursion is exercised with factorial; recursive calls execute in both backends. |
| Mutual recursion | Implemented | Mutually recursive even/odd functions are checked and executed in both backends. |
| Return on every required path | Implemented | Reachable fallthrough analysis covers sequential blocks, nested branches, early returns, loop exits, literal conditions/counts, and proven non-empty fixed-array/range iteration. |
| Early returns | Implemented | Nested and recursive early-return paths are covered by semantic and C/LLVM runtime tests. |
| Multiple return values | Not implemented | No tuple or multi-result return model. |
| Function overloading | Not implemented | Explicitly absent in current specification. |
| Default arguments | Not implemented | Design candidate only. |
| Named arguments | Not implemented | Design candidate only. |
| Variadic functions | Not implemented | Needs typed ABI and call semantics. |
| Generic functions | Not implemented | Requires generic type/constraint design. |
| Function values / pointers | Not implemented | No callable value type. |
| Closures and captures | Not implemented | Requires capture, lifetime and memory rules. |
| Anonymous functions / lambdas | Not implemented | Depends on callable values. |
| Higher-order functions | Not implemented | Depends on function values and generic collection APIs. |
| Foreign-function interface | Design undecided | ABI, calling conventions, symbol visibility and ownership across boundary need design. |
| Function visibility / public-private | Design undecided | Depends on module and package model. |
| Entry point | Implemented | Explicit zero-argument \`main\` is optional; implicit top-level entry is synthesized otherwise. |
| Multiple source files / cross-file calls | Not implemented | Current top-level function collection is within one compilation unit. |

## 2. Bindings, scopes and declarations

| Capability | Status | Audit notes / questions |
|---|---|---|
| Mutable local bindings | Implemented | \`let\`. |
| Immutable local bindings | Implemented | \`const\` prevents reassignment. |
| Local type inference | Implemented | Inferred from initializer. |
| Explicit local type annotations | Implemented | Primitive and fixed-array annotations such as `int[3]`; user-defined types remain absent. |
| Lexical block scopes | Implemented | Nested scopes and shadowing. |
| Duplicate declaration diagnostics | Implemented | Same-scope duplicates rejected. |
| Top-level executable statements | Implemented | Run in source order when explicit \`main\` is absent. |
| Global values accessible to functions | Not implemented | Top-level values belong to implicit entry execution and are not captured by ordinary functions. |
| Compile-time constants | Design undecided | \`const\` currently means immutable binding, not established compile-time evaluation. |
| Static/global storage | Not implemented | Needs initialization order and linkage rules. |
| Destructuring declarations | Not implemented | Depends on tuples, structs and pattern semantics. |
| Shadowing policy and diagnostics | Implemented | Existing lexical-scope behavior. |

## 3. Types and representation

| Capability | Status | Audit notes / questions |
|---|---|---|
| Signed 64-bit integer | Implemented | Current \`int\`. |
| 64-bit floating point | Implemented | Current \`float\`. |
| Boolean | Implemented | Current \`bool\`. |
| UTF-8 string | Partial | String values and output exist; full text API, ownership and Unicode behavior need audit. |
| Void | Implemented | Function no-value result. |
| Fixed-size one-dimensional arrays | Implemented | Primitive and structure arrays support inference, explicit `type[length]` annotations, structure fields, parameters/returns, and value aggregates. |
| Array indexing and mutation | Implemented | Runtime bounds checks; nested field/index writes rooted at named aggregates, in both backends. |
| Array iteration | Implemented | for-in over fixed and dynamic arrays of primitive or structure values. Dynamic length is checked at each loop header. |
| Array function parameters | Implemented | Fixed arrays of primitives or structures are passed by value; dynamic arrays are shared references. Element type, nominal structure identity, and shape are checked. |
| Array function returns | Implemented | Fixed arrays of primitives or structures are returned by value; dynamic arrays return a shared reference. IR verifies element identity and shape. |
| Nested arrays | Not implemented | Arrays of arrays remain unsupported; arrays of structures and array-valued structure fields are implemented. |
| Integer ranges | Implemented | \`start..end\` exclusive and \`start..=end\` inclusive, directly in \`for-in\`. |
| Unsigned integer types | Not implemented | Important for systems and binary data. |
| Smaller/larger integer widths | Not implemented | Consider explicit fixed-width types rather than implicit platform-sized integers. |
| 32-bit floating point | Not implemented | Useful for memory-sensitive and accelerator workloads. |
| Character / code-point type | Not implemented | Distinguish Unicode scalar values from bytes. |
| Byte / raw buffer type | Not implemented | Needed for binary I/O and networking. |
| User-defined structs / records | Implemented | Named value types with primitive, earlier-declared nested-struct, and one-dimensional array fields; arrays of structs support fixed value semantics and dynamic shared-reference semantics, nominal identity, nested field/index mutation, parameters/returns, push/pop/iteration, typed IR, C and LLVM backends. Recursive dynamic-array fields are supported; recursive by-value fields, explicit layout controls and equality remain deferred. |
| Tuples | Not implemented | Could support multiple results and grouped values. |
| Enums / tagged unions | Not implemented | Should be designed with exhaustive matching. |
| Union / untagged union | Not implemented | Low-level representation and safety rules needed. |
| Optional / nullable values | Not implemented | Decide representation and null-safety rules. |
| Result / error values | Not implemented | Needed for explicit recoverable error handling. |
| Dynamic arrays / vectors | Implemented | Primitive or structure elements, contiguous heap storage, geometric growth, bounds-checked indexing, `length`, `push`, `pop`, shared-reference aliasing, and entry-point cleanup. Arrays of arrays, slices, per-scope release, and borrowing checks remain absent. |
| Slices / views | Not implemented | Requires bounds, lifetime and aliasing rules. |
| Multidimensional arrays | Not implemented | Important for scientific computing; decide contiguous layout and indexing. |
| Maps / dictionaries | Not implemented | Hashing, equality, key constraints and allocation needed. |
| Sets | Not implemented | Usually built on hashing/equality and collection infrastructure. |
| Generic types | Not implemented | Needs type identity, constraints and monomorphization or another implementation strategy. |
| Interfaces / traits | Not implemented | Decide static vs dynamic dispatch and coherence rules. |
| Type aliases / newtypes | Not implemented | Useful for units and domain-specific safety. |
| Opaque / abstract types | Not implemented | Depends on modules and visibility. |
| Type inference beyond locals | Partial | Function parameters are explicit; generic inference is absent. |
| User-defined conversions | Not implemented | Avoid implicit surprising conversions. |
| Explicit casts / checked narrowing | Partial | int-to-float widening exists; a complete cast system does not. |
| Structure field order and target-native layout | Implemented | Fields retain declaration order; native target ABI alignment may insert inter-field and tail padding. C uses compiler layout/`sizeof`; LLVM uses native aggregate allocation size. Mixed-alignment records, nested fixed arrays, padded dynamic-array strides, growth, and C/LLVM behavior are integration-tested. Sizes and offsets are target-dependent. |
| Explicit packed / aligned layout controls | Not implemented | User-selected packing, alignment and ABI annotations remain deferred until a concrete FFI or binary-layout requirement is designed. |

## 4. Expressions, operators and evaluation

| Capability | Status | Audit notes / questions |
|---|---|---|
| Integer and floating arithmetic | Implemented | Checked signed integer arithmetic. |
| Comparisons and equality | Implemented | Primitive-compatible comparisons. |
| Boolean logic | Implemented | Includes short-circuit lowering. |
| Unary operators | Implemented | Unary plus, minus and logical not. |
| Assignment | Implemented | Local assignment and nested aggregate field/index updates rooted at named bindings; compound targets resolve each indexed path component exactly once. |
| Compound assignment | Implemented | \`+= -= *= /= %=\`. |
| Function-call expressions | Implemented | Direct calls. |
| Array indexing expressions | Implemented | Checked indexing. |
| Grouping / multiline expression form | Implemented | Explicit square-bracket grouping behavior is documented. |
| Bitwise AND / OR / XOR / NOT | Not implemented | Important for systems, cryptography and binary protocols. |
| Bit shifts and rotates | Not implemented | Need defined behavior for negative counts and overflow. |
| Modulo semantics for negative operands | Implemented | Truncating signed remainder; non-zero result follows the dividend's sign; `INT64_MIN % -1` is defined as zero. |
| Increment / decrement | Deferred | Avoid duplicating \`+= 1\` and \`-= 1\` unless a concrete need appears. |
| Ternary / conditional expression | Not implemented | Could be unnecessary if expression-oriented \`if\` is later adopted. |
| Null-coalescing / optional operators | Not implemented | Depends on optional values. |
| Range expressions as values | Not implemented | Current ranges are only accepted directly by \`for-in\`. |
| Slicing expressions | Not implemented | Depends on slices. |
| Struct field access | Implemented | Named and nested field access, including fields reached through array indexing. |
| Pattern matching expressions | Not implemented | Depends on enums and patterns. |
| Operator overloading | Deferred | Avoid until generics and value semantics are stable. |
| Explicit casts | Partial | Full conversion matrix and checked/unchecked rules remain. |
| Evaluation order | Implemented | Left-to-right order is specified and tested across binary operands, call arguments, arrays, indexing, range bounds and short-circuit logic in both backends. |
| Integer overflow behavior | Partial | Checked arithmetic exists; audit all operations and constant folding. |
| Floating-point edge behavior | Verify | NaN, infinities, signed zero and backend consistency need tests/docs. |

## 5. Control flow and program structure

| Capability | Status | Audit notes / questions |
|---|---|---|
| if / else | Implemented | Boolean condition required. |
| Counted repeat loop | Implemented | Integer count. |
| while loop | Implemented | Boolean condition required. |
| for-in over arrays | Implemented | Fixed and dynamic arrays; dynamic length is checked on each loop header. |
| for-in over integer ranges | Implemented | Exclusive and inclusive forms. |
| break / continue | Implemented | Innermost enclosing loop. |
| Nested loops | Implemented | Nested `while`, `repeat` and `for-in` loops target the innermost enclosing loop; inner `break`/`continue` are covered by C and LLVM runtime tests. |
| Labeled break / continue | Not implemented | Decide only if nested control-flow use cases justify it. |
| Switch / match | Not implemented | Prefer one exhaustive pattern construct over duplicate switch/match systems. |
| Pattern matching | Not implemented | Depends on enums, optionals and destructuring. |
| Guard / early-exit statement | Deferred | Could be expressed with if/return; assess later. |
| Defer / scope cleanup | Design undecided | Must define behavior on return, break, continue and runtime errors. |
| Exceptions | Design undecided | Not selected as default error model. |
| Assertions / panic / trap | Partial | Runtime failures exist for checked operations; public assertion API not established. |
| Unreachable / compiler assumptions | Not implemented | Needs safe semantics and optimizer contract. |
| Coroutines / generators | Not implemented | Depends on runtime and resumable function model. |
| Async / await | Not implemented | Depends on concurrency and cancellation model. |

## 6. Memory, ownership and resource safety

| Capability | Status | Audit notes / questions |
|---|---|---|
| Value semantics for primitives | Implemented | Primitive values flow through SSA. |
| Value semantics for fixed arrays | Implemented | Aggregate values; no heap allocation required. |
| Automatic local storage | Partial | Compiler/backend local representation exists; complete lifetime model is not defined. |
| Stack allocation policy | Design undecided | Define escape behavior and large-object handling. |
| Heap allocation | Not implemented as a VEX language API | Runtime/backend internals are not a user-facing allocator model. |
| Explicit allocators | Not implemented | Important for predictable systems/scientific workloads. |
| Deallocation | Not implemented | Requires ownership and lifetime rules. |
| Ownership | Design undecided | Hybrid direction selected, exact rules open. |
| Borrowing / references | Design undecided | Aliasing and mutability rules required. |
| Raw pointers | Not implemented | Safety boundaries and pointer arithmetic need explicit design. |
| Deterministic destruction | Design undecided | Must define drop order and control-flow interactions. |
| RAII / resource guards | Not implemented | Depends on destruction semantics. |
| Arena / pool allocation | Not implemented | Should be possible without making ordinary code verbose. |
| Alignment / placement allocation | Not implemented | Relevant to hardware and FFI. |
| Atomics / memory ordering | Not implemented | Depends on concurrency and memory model. |
| Garbage collection | Deferred | No tracing collector selected; do not claim it is ruled out permanently. |
| Reference counting | Design undecided | Evaluate cost, cycles and concurrency before choosing. |

## 7. Errors, safety and contracts

| Capability | Status | Audit notes / questions |
|---|---|---|
| Lexer/parser diagnostics | Implemented | Structured codes and source spans. |
| Static type diagnostics | Implemented | Semantic checker. |
| IR verifier diagnostics | Implemented | Verifies typed IR and control-flow invariants. |
| Runtime checked integer arithmetic | Implemented | Overflow and invalid division cases trap/fail. |
| Runtime array bounds checks | Implemented | C and LLVM integration coverage. |
| Recoverable error return type | Not implemented | Result-like type not available. |
| Error propagation syntax | Not implemented | Design after Result semantics. |
| Exceptions | Design undecided | Must not conflate with process traps. |
| Assertions | Not implemented as a standard language facility | Separate from compiler internal checks. |
| Preconditions / postconditions | Not implemented | Could be library/compiler feature later. |
| Sanitizer integration | Deferred | Tooling quality milestone. |
| Undefined behavior policy | Design undecided | Define guarantees for overflow, pointers, aliasing and unsafe operations. |

## 8. Strings, bytes and text

| Capability | Status | Audit notes / questions |
|---|---|---|
| String literals | Implemented | Double-quoted literals and documented escapes. |
| Embedded NUL preservation | Implemented | Length-aware backend string representation. |
| String output | Implemented | Via \`say\`. |
| String concatenation | Not implemented | Binary `+` is restricted to numeric operands in semantic analysis; typed IR arithmetic and both backends also define arithmetic only for numeric values. No string-concatenation allocation/runtime operation exists. |
| String equality | Implemented | Primitive equality is supported; test byte/Unicode semantics. |
| UTF-8 validation policy | Design undecided | Decide whether strings guarantee valid UTF-8 or are arbitrary bytes. |
| Unicode scalar iteration | Not implemented | Must not confuse bytes with code points or grapheme clusters. |
| String interpolation | Not implemented | Open design question. |
| Formatting / interpolation library | Not implemented | Needed for practical diagnostics and scientific output. |
| String builder / mutable buffer | Not implemented | Depends on allocation and ownership. |
| Split / join / trim / replace | Not implemented | Standard-library work. |
| Byte arrays / byte slices | Not implemented | Important for file and network APIs. |
| Encoding / decoding | Not implemented | Standard-library work. |

## 9. Runtime, operating system and standard library

| Capability | Status | Audit notes / questions |
|---|---|---|
| Terminal output | Partial | \`say\` only; formatted output and output streams absent. |
| Terminal input | Not implemented | Standard input API absent. |
| File read/write | Not implemented | Runtime and resource model absent. |
| Directory / filesystem APIs | Not implemented | Paths, metadata and iteration absent. |
| Process arguments | Not implemented | Needed for real command-line tools. |
| Environment variables | Not implemented | Runtime API absent. |
| Exit status control | Partial | C entry adaptation supports integer result; user-facing process API needs tests/spec. |
| Time / clocks / durations | Not implemented | Standard library. |
| Random number generation | Not implemented | Seedable deterministic API needed. |
| Networking | Not implemented | TCP/UDP/DNS/HTTP APIs absent. |
| Serialization | Not implemented | JSON/CSV/binary formats absent. |
| Math intrinsics | Partial | Current built-ins: sqrt, sin, cos, pow, abs. |
| Broader math library | Not implemented | Logs, exponentials, inverse trig, rounding, constants and numeric utilities. |
| Linear algebra | Not implemented | High priority for scientific computing, likely library/package layer. |
| Complex numbers | Not implemented | Decide library type vs primitive support. |
| Units / dimensional analysis | Not implemented | Valuable for physics/engineering; likely library plus optional type support. |
| Algorithms / sorting / searching | Not implemented | Standard library. |
| Collections library | Not implemented | Depends on dynamic collection primitives. |
| Testing framework | Partial | Compiler tests exist; user-facing VEX test framework absent. |
| Benchmarking | Not implemented | Tooling / standard library. |

## 10. Concurrency and parallel computing

| Capability | Status | Audit notes / questions |
|---|---|---|
| Threads | Not implemented | Runtime support absent. |
| Mutexes / locks | Not implemented | Depends on runtime and memory model. |
| Condition variables | Not implemented | Runtime support absent. |
| Atomics | Not implemented | Memory ordering model needed. |
| Channels / message passing | Not implemented | Design candidate. |
| Tasks / structured concurrency | Not implemented | Design candidate. |
| Async I/O | Not implemented | Depends on runtime and cancellation semantics. |
| SIMD / vector operations | Not implemented | Important for numerical workloads; decide language vs compiler/library API. |
| GPU / accelerator compute | Deferred | Long-term scientific computing capability; not core syntax milestone. |
| Parallel loops | Not implemented | Needs data-race and reduction semantics. |

## 11. Modules, packages and interoperability

| Capability | Status | Audit notes / questions |
|---|---|---|
| Multiple-file compilation | Not implemented | Current compiler is single-compilation-unit oriented. |
| Module declarations | Deferred | Intentionally postponed until core types and APIs stabilize. |
| Imports | Deferred | Same; avoid designing package syntax prematurely. |
| Visibility / export rules | Design undecided | Needs module boundary. |
| Package manager | Not implemented | Dependency resolution, versions and registries. |
| Build manifest | Not implemented | Build configuration and reproducibility. |
| C ABI / FFI | Design undecided | Important for scientific and systems libraries. |
| C library linking | Partial | Toolchain integration exists; VEX-level FFI not established. |
| Dynamic library loading | Not implemented | Runtime/OS API. |
| Cross-compilation | Partial | CI targets multiple OS/architectures; user-facing target selection and sysroot strategy need audit. |

## 12. Compiler, developer experience and distribution

| Capability | Status | Audit notes / questions |
|---|---|---|
| Source manager and source spans | Implemented | Byte-based spans and line mapping. |
| Lexer / parser / AST | Implemented | Current grammar subset. |
| Semantic analysis | Implemented | Current type/name rules. |
| Typed SSA IR | Implemented | With verifier. |
| C backend | Implemented | Bootstrap backend. |
| LLVM backend | Implemented | Textual LLVM IR and native execution path. |
| Cross-platform CI | Implemented | Ubuntu, Windows, macOS Intel and Apple Silicon matrix. |
| Unit / integration tests | Implemented | Compiler and both backend integration tests. |
| VS Code syntax highlighting | Partial | Current grammar; needs ongoing sync with language grammar. |
| VS Code Run integration | Implemented | Existing extension path. |
| Formatter | Not implemented | Important before syntax grows. |
| Linter | Not implemented | Could build on semantic diagnostics. |
| Debugger integration | Not implemented | Debug info and source mapping absent. |
| Language server / completion | Not implemented | Beyond current extension. |
| REPL | Not implemented | Not essential for systems language, but useful for exploration. |
| Package update mechanism | Deferred | Distribution polish milestone. |
| Reproducible builds | Partial | Pinned bundled toolchain; audit deterministic output and build metadata. |
| Optimization levels | Partial | LLVM may optimize through toolchain; VEX-level stable optimization policy needs specification. |
| Compile-time evaluation | Not implemented | Needs bounded execution and safety model. |
| Documentation generation | Not implemented | Could follow modules and public API metadata. |
| Stable language / ABI compatibility | Not implemented | Experimental 0.x language; compatibility policy needed before 1.0. |

## 13. Design principles and deliberate non-goals to review

These are decisions to make consciously, not features to add automatically:

- Prefer one coherent construct for each job; do not add duplicate loop, selection or error-handling syntax without a demonstrated use case.
- Keep common programs concise, but expose allocation, numeric widths, layout and concurrency controls where they materially affect performance or correctness.
- Avoid implicit conversions that can silently lose data.
- Keep compiler syntax small; put general algorithms, filesystem, networking and numerical tools in libraries.
- Do not add function overloading, operator overloading, exceptions, garbage collection, or a self-hosting requirement merely because another language has them.
- Preserve a clear escape hatch for low-level systems work.
- Treat scientific computing as a first-class workload: multidimensional data, numerical precision, reproducibility, linear algebra, parallelism and performance matter.

## 14. Audit gates before choosing the next implementation

1. **Verify existing claims:** add focused tests for string operations, negative modulo behavior and floating-point edge cases. Recursion, mutual recursion, reachable return-path analysis, nested loop control and evaluation order now have dedicated coverage.
2. **Resolve foundational designs:** structs/layout, array-of-struct representation, dynamic arrays/slices, optional/result errors, memory ownership and resource cleanup.
3. **Map dependencies:** do not implement a feature before its required type, lifetime, IR and backend semantics are understood.
4. **Select a coherent milestone:** choose a group of related features that can be completed end to end, not a scattered set of syntax additions.
5. **Keep CI broad:** preserve the four-platform matrix and test both C and LLVM backends.
6. **Update this matrix with every patch:** statuses and notes must reflect tested behavior, not aspirations.

## 15. Current known baseline

The current \`dev\` baseline includes primitive static typing, inferred local bindings, functions with typed parameters and optional return annotations, forward calls, optional explicit \`main\`, lexical scopes, conditionals, repeat/while/for-in loops, break/continue, compound assignment, checked primitive arithmetic, strings and \`say\`, fixed primitive arrays, array indexing/mutation/bounds checks, integer range iteration, math intrinsics, typed SSA IR, C and LLVM backends, CLI build/run tools, cross-platform tests and a VS Code extension.

This baseline does **not** imply that every edge case in the tables has been audited. Items marked **Verify** must be tested before being promoted to **Implemented**.
