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
| Function values / function pointers | Not implemented | No callable value type; this is distinct from the partially implemented data-pointer model below. |
| Closures and captures | Not implemented | Requires capture, lifetime and memory rules. |
| Anonymous functions / lambdas | Not implemented | Depends on callable values. |
| Higher-order functions | Not implemented | Depends on function values and generic collection APIs. |
| Foreign-function interface | Design undecided | ABI, calling conventions, symbol visibility and ownership across boundary need design. |
| Function visibility / public-private | Implemented | `pub make` exports a function from its declaring module; unmarked functions are private and imported calls must be qualified through an alias. |
| Entry point | Implemented | Explicit zero-argument \`main\` is optional; implicit top-level entry is synthesized otherwise. |
| Multiple source files / cross-file calls | Implemented | Explicit source files form one root module; recursive imports form separate module identities. The complete module graph is resolved before semantic checking and lowered into one IR module. Cached separate-object compilation remains open; `lex` and `parse` inspect only the named file. See [modules and import resolution](import-resolution.md). |

## 2. Bindings, scopes and declarations

| Capability | Status | Audit notes / questions |
|---|---|---|
| Mutable local bindings | Implemented | \`let\`. |
| Immutable local bindings | Implemented | \`const\` prevents reassignment. |
| Local type inference | Implemented | Inferred from initializer; a declaration without an initializer requires an explicit type. |
| Typed scalar default initialization | Implemented | Mutable explicitly typed scalar bindings default to 0, 0.0, false, or the empty string; arrays, structures, and const bindings still require explicit initializers. |
| Explicit local type annotations | Implemented | Primitive, nominal structure, fixed-array, and dynamic-array annotations such as `Point`, `Point[3]`, and `Point[]`. |
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
| String type | Partial | Strings are length-aware byte sequences, not guaranteed-valid UTF-8. Literals, exact byte length, equality, output, byte-preserving concatenation, and checked joining are implemented; Unicode-aware text operations remain unfinished. |
| Void | Implemented | Function no-value result. |
| Fixed-size one-dimensional arrays | Implemented | Primitive and structure arrays support inference, explicit `type[length]` annotations, structure fields, parameters/returns, and value aggregates. |
| Array indexing and mutation | Implemented | Runtime bounds checks; nested field/index writes rooted at named aggregates, in both backends. |
| Array iteration | Implemented | for-in over fixed/dynamic one-dimensional primitive, structure, and enum values plus supported nested jagged arrays; nested loops preserve the remaining element shape. The iterable is evaluated once; fixed arrays use the captured value, while dynamic arrays retain the shared header and re-read length/current storage each iteration. Alias push/pop/indexed writes/reserve affect later iterations; rebinding the source variable does not retarget the loop. Covered by C and LLVM runtime tests. |
| Array function parameters | Implemented | Fixed one-dimensional arrays are passed by value; dynamic and mixed nested jagged arrays preserve each dimension's value/reference representation. Element type, nominal structure/enum identity, and complete supported shape are checked. Dynamic enum arrays are exercised through parameters. |
| Array function returns | Implemented | Fixed one-dimensional arrays are returned by value; dynamic and mixed nested jagged arrays preserve each dimension's value/reference representation. IR verifies leaf identity and the complete supported shape, including dynamic enum-array returns. |
| Nested arrays | Implemented | Dynamic, mixed, and all-fixed jagged arrays are implemented end to end. Nested fixed storage is bounded by a target-native 256 KiB per-object limit and a 512 KiB per-function nested-fixed aggregate budget. C uses `sizeof` static assertions; LLVM checks target-layout allocation sizes before stack allocation. Contiguous rectangular multidimensional arrays remain a separate feature. See the [design proposal](nested-array-design.md). |
| Integer ranges | Implemented | \`start..end\` exclusive and \`start..=end\` inclusive, directly in \`for-in\`. |
| Unsigned integer types | Partial | `byte` is an unsigned 8-bit scalar; broader unsigned integer types are not yet language types. |
| Smaller/larger integer widths | Partial | `byte` provides 8-bit storage and `int` is signed 64-bit; independent 16/32/unsigned integer scalar types remain absent. |
| 32-bit floating point | Not implemented | Useful for memory-sensitive and accelerator workloads. |
| Character / code-point type | Not implemented | Distinguish Unicode scalar values from bytes. |
| Byte / raw buffer type | Implemented | `byte` is an unsigned 8-bit scalar; dynamic `byte[]` arrays provide mutable contiguous buffers with checked indexing, growth, capacity and reservation. |
| User-defined structs / records | Implemented | Named value types with primitive, earlier-declared nested-struct, declared-enum, and fixed/dynamic nested-array fields. Structure copies recursively copy nested structures and fixed dimensions while preserving each dynamic dimension's shared-header identity; native C/LLVM tests cover copied `int[][]`, mixed `int[2][]`/`int[][2]` fields, structure parameters/returns, arrays of structures, and recursive dynamic fields including self- and mutually-referential `Node[]` graphs through normal cleanup. Nominal identity, nested field/index mutation, push/pop/iteration, typed IR, and target-native layout are covered. Recursive by-value fields, explicit layout controls and equality remain deferred. |
| Tuples | Not implemented | Could support multiple results and grouped values. |
| Enums / tagged unions | Implemented | Nominal enum values and unit/payload constructors; payloads preserve primitive, struct, enum, and array identity; exhaustive statement-form `match` supports immutable scoped payload bindings and a final wildcard; typed IR verification, shared target layout, and C/LLVM construction, tag dispatch, and guarded payload access. Native C/LLVM integration executes unit, primitive, structure, fixed-array, dynamic-array, and string payloads, enum fields, fixed arrays of enums inside structures, and dynamic enum arrays through push/pop/reserve, aliases, append-during-iteration, and function parameters/returns. Expression-valued matches and richer patterns remain deferred. |
| Union / untagged union | Deferred | Kept separate from safe tagged enums; requires explicit unsafe access and layout rules. |
| Optional / nullable values | Not implemented | Decide representation and null-safety rules. |
| Result / error values | Not implemented | Needed for explicit recoverable error handling. |
| Dynamic arrays / vectors | Implemented | Primitive, structure, or enum values as one-dimensional elements, plus fixed-array and dynamic-array elements for supported nested shapes; contiguous heap storage per dynamic dimension; geometric growth; bounds-checked indexing; `length`, `capacity`, absolute-minimum `reserve`, `push`, `pop`; shared-header aliasing; and entry-point cleanup. Native C/LLVM tests cover dynamic arrays nested in copied structures, mixed fixed/dynamic field aliasing, and cleanup of self-/mutually-referential structure graphs. Borrowed slices, per-scope release, and borrowing checks remain absent; the separate `byte_slice` helper returns an owning copy. |
| Dynamic-array capacity and reservation | Implemented | `capacity` reports allocated element slots; `reserve(array, n)` treats `n` as an absolute minimum, preserves contents and length, never shrinks, and updates all shared aliases. |
| Chunk-based string building | Implemented | A StringBuilder structure can store string[] chunks, append with push, and materialize an immutable snapshot with join; joining validates length overflow and copies each output byte once. |
| Owning byte slices | Implemented | `byte_slice(byte[], start, count)` returns an independent copy with checked bounds; it does not expose a borrowed view. |
| Borrowed / zero-copy slices | Not implemented | Requires explicit lifetime, aliasing and invalidation rules. |
| Contiguous multidimensional arrays | Not implemented | Jagged nested arrays are implemented separately; a rectangular contiguous matrix/view still needs explicit shape, storage order, stride, and indexing semantics. |
| Maps / dictionaries | Not implemented | Hashing, equality, key constraints and allocation needed. |
| Sets | Not implemented | Usually built on hashing/equality and collection infrastructure. |
| Generic types | Not implemented | Needs type identity, constraints and monomorphization or another implementation strategy. |
| Interfaces / traits | Not implemented | Decide static vs dynamic dispatch and coherence rules. |
| Type aliases / newtypes | Not implemented | Useful for units and domain-specific safety. |
| Opaque / abstract types | Not implemented | Depends on modules and visibility. |
| Type inference beyond locals | Partial | Function parameters are explicit; generic inference is absent. |
| User-defined conversions | Not implemented | Avoid implicit surprising conversions. |
| Explicit casts / checked narrowing | Partial | `int` widens to `float`; `to_byte(int)` is checked and `to_int(byte)` zero-extends. A complete conversion matrix and additional widths do not exist. |
| Structure field order and target-native layout | Implemented | Fields retain declaration order; native target ABI alignment may insert inter-field and tail padding. C uses compiler layout/`sizeof`; LLVM uses native aggregate allocation size. Mixed-alignment records, nested fixed arrays, padded dynamic-array strides, growth, and C/LLVM behavior are integration-tested. Sizes and offsets are target-dependent. |
| Explicit packed / aligned layout controls | Not implemented | User-selected packing, alignment and ABI annotations remain deferred until a concrete FFI or binary-layout requirement is designed. |

## 4. Expressions, operators and evaluation

| Capability | Status | Audit notes / questions |
|---|---|---|
| Integer and floating arithmetic | Implemented | Checked signed integer arithmetic. |
| Comparisons and equality | Implemented | Primitive-compatible comparisons. |
| Boolean logic | Implemented | Includes short-circuit lowering. |
| Unary operators | Implemented | Unary plus, minus, logical not, and bitwise NOT for `int` (64 bits) and `byte` (8 bits). |
| Assignment | Implemented | Local assignment and nested aggregate field/index updates rooted at named bindings; compound targets resolve each indexed path component exactly once. |
| Compound assignment | Implemented | `+= -= *= /= %=` work on named bindings and nested structure/array paths; target indices execute once before the right-hand side. `+=` also accepts two strings. |
| Function-call expressions | Implemented | Direct calls. |
| Array indexing expressions | Implemented | Checked indexing. |
| Grouping / multiline expression form | Implemented | Explicit square-bracket grouping behavior is documented. |
| Bitwise AND / OR / XOR / NOT | Implemented | `&`, `|`, `^`, and `~` operate on matching `int` or `byte` values; byte operations are exactly 8 bits. Lexer, precedence, semantic checks, typed IR verification, C/LLVM code generation, and integration execution tests cover both types. |
| Bit shifts and rotates | Implemented | Shifts and rotations support `int` and `byte` end to end. Shift counts outside the operand width trap; rotations normalize signed counts modulo 64 or 8 bits. C and LLVM integration tests cover sign behavior, wraparound, negative/oversized rotation counts, and both widths. |
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
| for-in over arrays | Implemented | The iterable is evaluated once. Fixed arrays use the captured value; dynamic arrays retain the shared header and re-read length/current storage at each loop header. Alias push/pop/indexed writes/reserve affect later iterations, while rebinding the source variable does not retarget the loop. C/LLVM runtime coverage includes primitive, nested, and enum arrays. |
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
| Function-local typed places, field/element projections, explicit load/store, and typed pointer targets | Implemented | Mutable local bindings and parameters have stable typed root places. Structure-field and array-element projections are verified for loads/stores, but cannot currently be addressed with `&`. Typed pointers with up to eight indirection levels support whole-binding address-of, dereference reads/writes including compound stores, pointer-cell rebinding, and nested aggregate projection through a dereferenced structure pointer in C and LLVM. Pointer-valued structure fields, enum payloads, and array elements are not representable in the current aggregate type descriptors, so projected pointer cells are not yet expressible. |
| Stack allocation policy | Partial | Nested fixed aggregate storage has target-native 256 KiB per-object and 512 KiB per-function limits; the general stack/heap model, scalar-frame accounting, recursion limits, and configurable budgets remain undefined. |
| Program-exit cleanup of compiler-tracked allocations | Implemented | C and LLVM track dynamic-array headers/buffers and allocated string blocks; both release them after normal entry return and before rejecting an out-of-range integer exit status. Traps terminate the process without language-level unwinding; the operating system reclaims memory. No per-scope, pop, replacement, or unreachable-alias release exists. |
| Heap allocation | Not implemented as a VEX language API | Runtime/backend internals are not a user-facing allocator model. |
| Explicit allocators | Not implemented | Important for predictable systems/scientific workloads. |
| Deallocation | Not implemented | Requires ownership and lifetime rules. |
| Ownership | Design undecided | Hybrid direction selected, exact rules open. |
| Borrowing / references | Design undecided | Aliasing and mutability rules required. |
| Raw pointers | Partial | Typed pointers up to eight indirection levels support whole-named-binding address-of, dereference, pointer parameters, directly returned borrowed pointer parameters, typed null constants, and pointer/null equality. Projected field/array loads and stores through a pointer-to-structure are supported, but taking the address of a projected place is not. Pointer-valued structure fields, enum payloads, and array elements are not representable yet; therefore projected pointer cells cannot currently be expressed. Existing lexical checks and interprocedural summaries cover direct local pointer cells and direct `**T` parameter stores through local aliases/forwarders/helper returns. Pointer arithmetic, integer casts, arbitrary alias/projection provenance, full lifetime analysis, and an explicit unsafe boundary remain incomplete. Dynamic-array element addresses additionally need a reallocation-invalidation rule. |
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
| Recoverable error return type | Not implemented | Result-like type not available. File I/O currently reports failure with the documented `-1` integer sentinel instead of a general error value. |
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
| String byte length | Implemented | `length(string)` reads the descriptor byte count; embedded NUL is included. It is not a Unicode character-count API. |
| String output | Implemented | Via \`say\`. |
| String concatenation | Implemented | Binary string `+` and string `+=` lower through a dedicated typed-IR operation; C and LLVM allocate one tracked block for each non-empty concatenation, preserve embedded NUL bytes, return the other operand directly for empty-input cases, and release tracked blocks at normal generated-entry return. Repeated dynamic `+=` remains quadratic; a chunk-based builder can be composed from `string[]`, `push`, and `join`, while a built-in mutable string buffer remains deferred. |
| String equality | Implemented | Primitive equality is supported; test byte/Unicode semantics. |
| UTF-8 validation policy | Implemented | Strings are length-aware byte sequences; no UTF-8 validation or normalization is performed. Unicode-aware text operations remain future work. |
| Unicode scalar iteration | Not implemented | Must not confuse bytes with code points or grapheme clusters. |
| String interpolation | Not implemented | Open design question. |
| Formatting / interpolation library | Not implemented | Needed for practical diagnostics and scientific output. |
| Built-in mutable string builder / buffer | Not implemented | No dedicated mutable string-buffer type or built-in; chunk-based building is already composable from `string[]`, `push`, and `join` as documented above. |
| Split / delimiter join / trim / replace | Not implemented | The existing `join(string[])` concatenates chunks without a delimiter; text splitting, delimiter-based joining, trimming and replacement remain future standard-library work. |
| Dynamic byte buffers | Implemented | `byte[]` uses the dynamic-array runtime, including `push`, `pop`, `reserve`, `capacity`, checked indexing, and alias sharing. |
| Owning byte slicing | Implemented | `byte_slice` copies a checked range into a separately owned dynamic byte array; borrowed slices remain unavailable. |
| Endian-aware integer encoding / decoding | Implemented | Checked 16-bit and 32-bit unsigned little-/big-endian read/write helpers operate on `byte[]`; no `u16`/`u32` scalar types are introduced. |
| UTF-8 and general text encoding / decoding | Not implemented | String validity, Unicode scalar iteration, and general text encoders/decoders remain future work. |

## 9. Runtime, operating system and standard library

| Capability | Status | Audit notes / questions |
|---|---|---|
| Terminal output | Partial | \`say\` only; formatted output and output streams absent. |
| Terminal input | Not implemented | Standard input API absent. |
| Whole-file binary read/write | Partial | `read_file(path, byte_array)` appends in chunks and returns bytes appended or `-1`; `write_file(path, byte_array)` truncates/writes and returns bytes written or `-1`. Both backends close the file before returning; streaming handles and richer `Result` errors are absent. |
| Directory / filesystem APIs | Not implemented | Path manipulation, metadata, directory iteration, and long-lived file handles are absent. |
| Process arguments | Not implemented | Needed for real command-line tools. |
| Environment variables | Not implemented | Runtime API absent. |
| Exit status control | Partial | C entry adaptation supports integer result; user-facing process API needs tests/spec. |
| Time / clocks / durations | Not implemented | Standard library. |
| Random number generation | Not implemented | Seedable deterministic API needed. |
| Networking | Not implemented | TCP/UDP/DNS/HTTP APIs absent. |
| Serialization | Partial | Primitive 16-/32-bit unsigned values can be read/written with explicit endianness in `byte[]`; structured formats, JSON/CSV, and general codecs are absent. |
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
| Multiple-file compilation | Implemented | Explicit roots and recursive imports resolve into one module graph and one typed IR program; C11 and LLVM native paths are exercised. Cached independent object compilation is not implemented. |
| Module declarations | Implemented | Each imported file has a canonical module identity and an independent namespace. Explicit CLI roots are grouped into one root module. |
| Module resolution and imported declarations | Implemented | Relative imports, optional aliases, default file-stem aliases, canonical deduplication, cycle termination, qualified calls/types/enums, and source-located diagnostics. |
| Visibility / export rules | Implemented | Functions, structures, and enums are private by default; `pub` exposes them to importing modules. Module-level executable statements are rejected in imported modules. |
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

1. **Verify existing claims:** add focused tests for floating-point edge cases, broader string/text behavior, and numeric boundary cases. Recursion, mutual recursion, reachable return-path analysis, nested loop control and evaluation order now have dedicated coverage.
2. **Resolve foundational designs still open:** borrowed slices, nested/multidimensional collections, optional/result errors, ownership/lifetimes, allocators, and deterministic resource cleanup.
3. **Map dependencies:** do not implement a feature before its required type, lifetime, IR and backend semantics are understood.
4. **Select a coherent milestone:** choose a group of related features that can be completed end to end, not a scattered set of syntax additions.
5. **Keep CI broad:** preserve the four-platform matrix and test both C and LLVM backends.
6. **Update this matrix with every patch:** statuses and notes must reflect tested behavior, not aspirations.

## 15. Current known baseline

The current `dev` baseline includes statically typed primitives and inferred locals; functions, forward and recursive calls, control flow and ranges; nominal structures and enums/tagged unions; fixed, dynamic, mixed, and all-fixed nested jagged arrays of primitive and structure values with target-aware storage budgets; nested aggregate reads, writes, iteration, collection operations, and compound assignment; byte-preserving string concatenation and chunk joining; unsigned `byte` buffers, owning byte slices, endian-aware binary codecs, and whole-file binary I/O; typed SSA IR with recursive array descriptors, function-local typed places, and an independent verifier; C11 and LLVM backends; native build/run paths; and partial typed pointers with up to eight indirection levels, parameter-borrowed returns, typed nulls, and conservative pointer-cell lifetime summaries. The source-level module system supports recursive relative imports, canonical module identity, aliases, independent namespaces, public/private function and nominal-type declarations, qualified access, and native linking through both backends. Cached separate-object compilation, build manifests, packages, and FFI remain open. Pointer-valued aggregate fields, projected address-of, complete pointer provenance/lifetime rules, borrowed slices, user-visible allocators, per-scope cleanup, contiguous rectangular arrays, generics, and FFI remain open.

The binary-buffer and file-I/O implementation is present end to end in the C and LLVM paths. It passed CI run [#271](https://github.com/Atiqxyz/vex/actions/runs/43743633949) on Ubuntu, Windows, macOS ARM, and macOS Intel at commit `e6286e1`. This does **not** mean VEX has borrowed/zero-copy slices, streaming file handles, a general `Result` type, a complete ownership/lifetime model, or all numeric widths. Items marked **Verify** still need focused tests before being promoted to **Implemented**.
