# VEX master roadmap and design map

**Purpose:** A durable map of the capabilities VEX should evaluate, the reasons they matter, the dependencies between them, and the design questions future implementation work must preserve.

**Project direction:** VEX is an independently developed general-purpose programming language with systems, computational, and scientific-computing ambitions. It should combine readable source code, predictable semantics, native performance, and meaningful low-level control. This is a design map, not a commitment to implement every item.

**Status authority:** The [language completeness matrix](language-completeness-matrix.md) tracks what is actually implemented and tested. The [language specification](../SPEC.md) records current syntax and semantic decisions. This roadmap records what to consider next and why. If these documents disagree, do not guess: reconcile them before implementation.

---

## 1. The principles every decision must preserve

1. **Performance without needless ceremony.** Common code should be concise; performance-sensitive code should expose the controls that matter.
2. **Predictable semantics.** Types, evaluation order, integer behavior, memory ownership, aliasing, and errors must have documented rules.
3. **Low-level capability.** VEX must eventually be able to express systems work, binary data handling, explicit memory strategies, and foreign interfaces.
4. **Strong computational foundations.** Numerical precision, arrays, multidimensional data, linear algebra, reproducibility, and parallel execution are important workloads, not afterthoughts.
5. **Readable syntax.** Prefer a small number of orthogonal constructs over many overlapping keywords and special cases.
6. **Portable behavior.** C and LLVM backends must implement the same VEX semantics. Platform differences belong behind explicit, documented boundaries.
7. **A coherent language, not a feature collection.** A feature is not automatically desirable because Rust, C++, Zig, Go, Python, or another language has it.
8. **Useful defaults with an escape hatch.** Safe and convenient defaults should not prevent expert users from controlling allocation, layout, precision, or execution when needed.
9. **Self-hosting capability, not a self-hosting mandate.** The language, runtime, and toolchain should eventually be expressive enough to implement VEX's own compiler. The compiler itself may remain written in C++; self-hosting is an architectural capability test, not a release requirement.

## 2. How to use other languages as references

Use other languages as evidence and design material, never as templates to copy wholesale.

| Design area | Useful references to study | Questions VEX should answer |
|---|---|---|
| Low-level data and ABI | C, C++, Zig | How are layout, alignment, pointers, and external calls exposed? |
| Ownership and resource lifetime | Rust, C++, Zig, Swift | How can resources be predictable without making ordinary code painful? |
| Collections and views | Rust, C++, Go, Swift | What are the semantics of vectors, slices, mutation, capacity, and invalidation? |
| Type composition | Rust, C, Zig, Swift, Kotlin | How should records, enums, optionals, and tagged unions compose? |
| Generic programming | Rust, C++, Go, Zig | Which constraints and implementation strategies give power without excessive compiler complexity? |
| Errors | Rust, Zig, Go, Swift | How are recoverable errors represented and propagated? |
| Modules and packages | Rust, Go, Python, C++ | How should visibility, imports, builds, and dependencies work? |
| Numerical computing | Fortran, Julia, C++, Rust, Python ecosystem | How do arrays, precision, vectorization, and libraries support real scientific workloads? |
| Concurrency | Go, Rust, C++, Swift | What memory model and task model can VEX explain and enforce? |
| Compiler architecture | Clang/LLVM, GCC, Zig | Which compiler boundaries make features reusable and backends consistent? |

For a significant feature, compare at least two approaches. Record what VEX adopts, what it rejects, and why. Prefer learning from language specifications and mature implementation documentation over copying surface syntax.

## 3. Capability map

### A. Core type system and data modelling

**Why it matters:** Real programs need to represent more than primitive numbers and primitive arrays. This is the foundation for scientific models, application state, algorithms, and APIs.

- [x] User-defined structures / records with named fields
- [x] Struct construction, field reads, and field mutation
- [x] Struct assignment, parameter passing, and return semantics
- [x] Defined field layout, padding, and alignment rules
- [x] Nested structures, arrays of structures, and array-valued structure fields
- [ ] Enums for finite named alternatives
- [ ] Tagged unions / algebraic data types
- [ ] Exhaustive matching over enums and tagged unions
- [ ] Tuples and multi-value returns where they simplify APIs
- [ ] Type aliases and distinct newtypes
- [ ] Optional values and explicit nullable representation
- [ ] Result-like recoverable error values
- [ ] Additional unsigned integer scalar types (`u16`, `u32`, `u64`, etc.)
- [ ] 32-bit and additional floating-point types where justified
- [x] Unsigned 8-bit `byte` scalar and byte arrays with defined semantics
- [ ] Character / Unicode scalar type
- [ ] Explicit casts, checked narrowing, and conversion rules
- [ ] Type inference beyond local initializers
- [ ] Generic types and constrained generic functions
- [ ] Static interfaces / traits or another coherent abstraction mechanism
- [ ] Opaque types and controlled representation visibility

**Design guardrails:** Decide value versus reference behavior explicitly. Decide whether enums are closed and exhaustively matched. Keep integer widths explicit rather than making platform-sized assumptions. Do not introduce classes or inheritance just because other languages do; plain data aggregates and composition may be enough.

### B. Arrays, collections, and data views

**Why it matters:** Collections are central to algorithms, simulations, numerical work, and general programming. Their memory and aliasing behavior directly affect performance and correctness.

- [x] Fixed-size one-dimensional primitive and structure arrays
- [x] Dynamic one-dimensional primitive and structure arrays
- [x] Dynamic and mixed fixed/dynamic nested arrays (jagged collections) across semantic analysis, recursive IR verification, lowering, C/LLVM backends, and native runtime tests; all-fixed nested aggregates remain gated; see [design proposal](nested-array-design.md)
- [x] All-fixed nested array aggregates with target-native object-size checks and a per-function nested-fixed storage budget (see [design proposal](nested-array-design.md))
- [ ] Contiguous multidimensional arrays with documented storage order and strides
- [x] Checked owning byte-range copies through `byte_slice`
- [ ] Slices / non-owning views over arrays and buffers (requires lifetimes and aliasing rules)
- [ ] Array views with explicit shape and stride where useful
- [x] Capacity inspection and reservation
- [ ] Resize, insert, remove, clear, and efficient bulk operations
- [ ] Iterators with defined mutation and invalidation behavior
- [ ] General iteration protocol, if it provides real composability
- [ ] Maps / dictionaries
- [ ] Sets
- [ ] Deques, queues, stacks, and other useful collection forms
- [ ] Generic collections rather than separate primitive-only implementations

**Design guardrails:** Define ownership, aliasing, mutation, capacity growth, invalidation, and allocator behavior before expanding the API. For multidimensional numerical arrays, compare contiguous row-major/column-major layouts and explicit strides. Avoid building several incompatible collection protocols.

### C. Expressions, operators, and language grammar

**Why it matters:** The grammar determines how naturally users can express algorithms. Additions should improve capability without creating overlapping syntax.

- [x] Bitwise AND, OR, XOR, NOT for matching `int` and `byte` operands
- [x] Bit shifts and rotations with defined edge behavior: checked shift counts, arithmetic signed right shift, logical byte right shift, fixed-width wraparound left shift, and modulo-width rotations
- [x] Struct field access and nested field/index expressions
- [x] Nested assignment targets across fields and indexes when rooted at a named aggregate binding
- [x] Compound assignment operators on named bindings and nested fields/indexes, with exactly-once target evaluation
- [ ] Range values, if they become useful beyond direct loops
- [ ] Slicing syntax
- [ ] Conditional expressions or expression-valued if, choose one coherent model
- [ ] Match expressions integrated with enums and patterns
- [ ] Destructuring for tuples, records, and patterns
- [ ] Explicit conversion/cast syntax and checked conversion operations
- [ ] Compile-time expressions and constant evaluation
- [ ] Function overloading only if type resolution remains predictable
- [ ] Default, named, and variadic arguments only where they improve APIs
- [ ] Function values, function pointers, lambdas, and closures
- [ ] Operator overloading only after generic constraints and value semantics stabilize
- [ ] User-defined literal syntax only if a concrete domain requires it

**Design guardrails:** Preserve newline statement separation and brace-delimited blocks unless there is a compelling reason to change them. Avoid adding multiple ways to express the same control flow. Keep punctuation and precedence consistent.

### D. Control flow and error handling

**Why it matters:** Programs need clear branching, early exits, recoverable failures, and reliable cleanup.

- [ ] A single expressive match / pattern-selection construct
- [ ] Pattern matching for enums, optionals, tuples, and records
- [ ] Guards / early-exit convenience, if it improves clarity
- [ ] Scope-exit cleanup / defer semantics
- [ ] Assertions and explicit panic/trap APIs
- [ ] Result-based recoverable errors
- [ ] Error propagation syntax or a simple explicit alternative
- [ ] Error context and chaining conventions
- [ ] Optional-value handling and null-safe operations
- [ ] Contracts such as preconditions and postconditions, possibly as tooling
- [ ] Generators / resumable functions if a real use case appears

**Design guardrails:** Keep recoverable errors distinct from fatal traps. Define cleanup behavior on normal return, early return, break, continue, and failure. Do not select exceptions by default merely because they are common elsewhere.

### E. Memory, ownership, and resource management

**Why it matters:** VEX aims to be a systems language. Memory behavior cannot remain an undocumented implementation detail forever.

- [ ] A documented stack and heap model
- [x] Target-aware object and per-function limits for nested fixed aggregate stack storage
- [ ] User-visible allocator interface
- [ ] Allocator selection and allocator-aware collections
- [ ] Explicit allocation and deallocation where appropriate
- [ ] References and borrowing rules
- [ ] Raw pointers and pointer arithmetic behind clear safety rules
- [ ] Ownership and move/copy semantics
- [x] Program-exit cleanup of compiler-tracked dynamic-array headers/buffers and allocated string blocks in both backends
- [ ] Deterministic destruction and resource cleanup
- [ ] RAII-like resource management if it fits VEX's model
- [ ] Arena, pool, and region allocation
- [ ] Alignment-aware and placement allocation
- [ ] Escape analysis / stack promotion where beneficial
- [ ] Atomic operations and memory ordering
- [ ] Optional unsafe boundary for operations that cannot be statically guaranteed safe

**Design guardrails:** Do not bolt on ownership after collections and structs have spread inconsistent assumptions. Evaluate Rust borrowing, C++ RAII, Zig allocator passing, and reference-counting trade-offs. The goal is predictable lifetime and aliasing behavior, not copying one language's complete memory model.

### F. Numeric and scientific-computing foundations

**Why it matters:** Scientific computing is a first-class VEX workload. This category should influence the type system, collections, standard library, compiler optimization, and concurrency design.

- [ ] Complete numeric type and conversion matrix
- [ ] Documented integer overflow, division, remainder, and shift semantics
- [ ] Defined floating-point behavior for NaN, infinities, signed zero, and exceptional domains
- [ ] Expanded math library: logarithms, exponentials, inverse/hyperbolic trig, rounding, classification, stable helpers
- [ ] Mathematical constants with clear precision
- [ ] Complex numbers
- [ ] Vectors and matrices
- [ ] Multidimensional and strided numerical arrays
- [ ] Linear algebra: matrix products, decompositions, solvers, eigenvalues
- [ ] Statistics and random distributions
- [ ] Reproducible seeded random-number generation
- [ ] Numerical integration and differentiation
- [ ] Differential-equation and optimization utilities as libraries
- [ ] Units / dimensional analysis support, likely through library types and possibly compiler-assisted checking
- [ ] SIMD and vectorization
- [ ] Parallel numerical loops and reductions
- [ ] GPU / accelerator interoperability as a longer-term extension
- [ ] Benchmarking and numerical accuracy test utilities

**Design guardrails:** Keep core scalar types small. Put broad algorithms in libraries. Make precision, layout, reproducibility, and performance visible. Compare Fortran, Julia, C++ numerical libraries, Rust ecosystem approaches, and established BLAS/LAPACK-style interfaces before designing numerical APIs.

### G. Strings, bytes, and encoding

**Why it matters:** Systems programs need both human-readable text and exact binary data. These must not be confused.

- [x] Decide and document string encoding semantics: length-aware byte sequences with no UTF-8 validation guarantee
- [x] Mutable contiguous byte buffers through dynamic `byte[]`
- [x] Checked owning byte-range copies through `byte_slice`
- [ ] Borrowed / zero-copy byte slices (requires a memory-lifetime model)
- [ ] String formatting and interpolation
- [x] Byte-preserving string concatenation with `+` and `+=` across C and LLVM backends
- [x] Efficient chunk-based string building with one-pass join
- [ ] UTF-8 decoding and validation policy
- [ ] Unicode scalar iteration, with bytes clearly distinguished
- [ ] Common string operations: search, split, delimiter join, trim, replace
- [ ] Parsing and formatting numeric values
- [ ] Encoding and decoding APIs
- [x] Unsigned 16-/32-bit little- and big-endian readers and writers over `byte[]`

**Design guardrails:** Define whether indexing counts bytes, Unicode scalar values, or something else. Do not silently treat grapheme clusters as single bytes. Keep arbitrary binary buffers separate from text strings. Do not expose mutable string storage until aliasing, lifetime, and ownership are explicit; an efficient builder should have its own predictable mutation and snapshot semantics.

### H. Runtime, operating system, and standard library

**Why it matters:** A language becomes useful when programs can interact with their environment without every project inventing its own wrappers.

- [ ] Formatted output and output streams
- [ ] Standard input and line reading
- [x] Whole-file binary read/write helpers with deterministic close in C and LLVM backends
- [ ] Long-lived file handles, buffered streams, and directory/filesystem APIs
- [ ] Paths and filesystem metadata
- [ ] Process arguments and environment variables
- [ ] Exit codes and process control
- [ ] Time, clocks, durations, and timers
- [ ] Random generators
- [ ] Networking primitives and higher-level protocols
- [x] Primitive 16-/32-bit unsigned binary integer codecs with explicit endianness
- [ ] Structured binary serialization, JSON, and CSV formats
- [ ] Sorting, searching, and general algorithms
- [ ] Collection utilities and iterators
- [ ] Testing and assertion library
- [ ] Benchmarking library
- [ ] Logging and diagnostics APIs
- [ ] Platform abstractions with explicit escape hatches

**Design guardrails:** Use explicit error results for OS operations. Resource handles must close deterministically. Distinguish monotonic time from wall-clock time. Avoid putting every library function into the compiler as a special intrinsic.

### I. Graphics, windows, and interactive applications

**Why it matters:** VEX should eventually be able to build real-time interactive programs, games, visual tools, and scientific visualisations—not only command-line software or static image files. This is a first-class application domain, while remaining outside the minimal language core.

- [ ] Window creation, lifecycle, and cross-platform event loop
- [ ] Keyboard, pointer, controller, and window input events
- [ ] 2D drawing primitives, textures, sprites, and text
- [ ] Frame timing, animation scheduling, and deterministic update loops
- [ ] GPU resource abstractions for buffers, textures, and pipelines
- [ ] Shader compilation, loading, and diagnostics
- [ ] 3D rendering foundations: meshes, cameras, transforms, materials, and lighting
- [ ] Audio playback and input as an optional multimedia subsystem
- [ ] Asset loading, resource lifetime, and packaging
- [ ] Scientific visualisation utilities for plots, fields, trajectories, and simulation state
- [ ] Headless and software-rendered test paths for CI
- [ ] Backend strategy for platform graphics APIs and third-party rendering libraries

**Design guardrails:** Keep graphics APIs in libraries and runtime/platform layers rather than making rendering operations language keywords. Separate the portable VEX-facing API from window-system and GPU backend details. Define resource ownership, thread affinity, frame/event-loop behavior, and error handling before exposing handles. Keep a headless path so compiler and simulation tests do not require a physical GPU or display. Do not commit prematurely to one graphics API; compare established cross-platform windowing and rendering options when implementation begins.

### J. Modules, packages, builds, and interoperability

**Why it matters:** Multi-file programs and reuse require a compilation and dependency model, not just import syntax.

- [ ] Multi-file compilation and cross-file name resolution
- [ ] Modules, imports, and exports
- [ ] Public/private visibility
- [ ] Namespace and package identity rules
- [ ] Separate compilation and linking model
- [ ] Build manifest and reproducible build configuration
- [ ] Dependency/package manager and versioning
- [ ] Static and dynamic library linking
- [ ] C ABI and foreign-function interface
- [ ] Calling conventions and data-layout compatibility
- [ ] C header / binding generation or a maintainable binding workflow
- [ ] Dynamic library loading if justified
- [ ] Cross-compilation targets, sysroots, and target-specific configuration

**Design guardrails:** Decide module identity, visibility, and build behavior together. FFI must define ownership and error boundaries, not only symbol names. Keep the compiler frontend independent from a particular package manager.

### J. Concurrency and parallel execution

**Why it matters:** Simulation, data processing, and systems workloads benefit from safe parallelism, but concurrency affects the entire memory model.

- [ ] Thread creation and joining
- [ ] Mutexes, read/write locks, and condition variables
- [ ] Atomics and memory-ordering model
- [ ] Message passing / channels
- [ ] Structured task execution
- [ ] Cancellation and shutdown semantics
- [ ] Parallel loops and reductions
- [ ] Async I/O only if the runtime model justifies it
- [ ] SIMD as a separate data-parallel capability
- [ ] GPU and accelerator execution as a later extension

**Design guardrails:** Establish memory and ownership semantics before exposing broad concurrency. Avoid mixing async I/O, OS threads, SIMD, and GPU compute into one vague feature.

### K. Compiler and developer experience

**Why it matters:** Language capability is only useful if users can understand errors, inspect programs, debug them, and maintain code.

- [x] Keep source, lexer, parser, semantic analysis, IR, verifier, and backends cleanly separated
- [ ] Expand typed IR for every new semantic feature
- [ ] Keep backend semantics equivalent
- [ ] Strong diagnostics with source spans and actionable messages
- [ ] Constant folding and compile-time evaluation
- [ ] Optimization pipeline and documented optimization levels
- [ ] Debug information and debugger integration
- [ ] Formatter
- [ ] Linter and static analysis
- [ ] Language server, completion, hover, and navigation
- [ ] VS Code grammar and editor integration kept synchronized
- [ ] REPL only if it supports the intended workflow
- [ ] User-facing testing and benchmarking tools
- [ ] Documentation generation
- [ ] Compiler performance and memory profiling
- [ ] Fuzzing, sanitizers, and differential backend testing

**Design guardrails:** Keep syntax tooling and semantic tooling derived from the language specification where possible. Do not let backend-specific behavior leak into the language definition.

### L. Release engineering and ecosystem maturity

**Why it matters:** Distribution matters, but only after the language and toolchain are sufficiently stable.

- [ ] Stable language specification and compatibility policy
- [ ] Stable runtime and ABI policy
- [ ] Versioned standard library
- [ ] Reproducible compiler and toolchain builds
- [ ] Cross-platform installation and update workflow
- [ ] Native installers and package-manager distribution
- [ ] Cross-compilation validation
- [ ] Performance and compatibility benchmark suite
- [ ] Security and dependency review
- [ ] Release notes and migration guides
- [ ] 1.0 readiness audit

---

## 4. Suggested implementation order

Apply the [low-level foundations contract](low-level-foundations.md) throughout implementation. Memory representation, target layout, addressable storage, pointers, allocator identity, and lifetimes are cross-cutting design constraints; they must be built alongside dependent features, not postponed until after the feature roster.

This is a dependency-aware sequence, not a rigid promise. Reorder when source inspection or reference research reveals a better path.

### Milestone 1 — Data modelling foundation
1. Structures / records: declaration, construction, field access, mutation.
2. Struct semantics: value behavior, copy behavior, parameters, returns, nested fields.
3. [x] Arrays of structs and structs containing fixed/dynamic arrays, including nested field/index updates.
4. Struct layout and alignment decisions that are consistent across C and LLVM.
5. [Partial] Enum declarations, payload validation, and nominal IR type descriptors; continue with typed enum values, construction, and exhaustive matching (see [design](enum-and-tagged-union-design.md)).
6. Tuples and destructuring if they simplify multiple returns and data flow.

### Milestone 2 — Complete the core type and expression model
1. [x] Recursive nested jagged collections implemented; design contiguous rectangular multidimensional arrays separately (see [nested-array design](nested-array-design.md)).
2. Borrowed slices and views, after lifetime/aliasing rules are established (owning byte copies already exist).
3. Additional integer widths and floating-point variants (the 8-bit `byte` type already exists).
4. [x] Bitwise operators for `int` and `byte`; complete numeric conversion rules remain.
5. Optionals and Result-like values.
6. Generic functions and types, after real examples identify the necessary constraints.
7. Function values and closures only after their lifetime model is understood.

### Milestone 3 — Memory and systems control
1. Ownership, references, borrowing, and aliasing model.
2. Allocator interface and allocator-aware collections.
3. Explicit allocation, deallocation, and deterministic cleanup.
4. Raw pointers and carefully scoped unsafe operations.
5. Layout controls, atomics, and FFI foundations.

### Milestone 4 — Error handling and standard runtime
1. Recoverable error conventions and propagation.
2. Scope cleanup and resource guards.
3. Expand string APIs and byte-buffer operations (basic buffers, owning copies, endian codecs already exist).
4. Expand runtime APIs beyond whole-file binary read/write: streaming files, paths, process, environment, input/output, and time.
5. Random generation and core algorithms.

### Milestone 5 — Scientific and numerical libraries
1. Complete math and numeric edge semantics.
2. Matrix/vector and multidimensional array facilities.
3. Linear algebra and numerical algorithms.
4. Units, statistics, integration, and differential-equation libraries.
5. SIMD and performance-oriented numerical operations.

### Milestone 6 — Modules and ecosystem
1. Multi-file compilation.
2. Module visibility and imports.
3. Separate compilation and linking.
4. Build manifests and package/dependency management.
5. C ABI and foreign-library bindings.

### Milestone 7 — Concurrency and advanced execution
1. Memory model and atomics.
2. Threads, synchronization, and message passing.
3. Structured parallel tasks and parallel loops.
4. Async I/O if justified.
5. GPU/accelerator integration as an optional long-term direction.

### Milestone 8 — Compiler maturity and release
1. Formatter, linter, language server, debugger, and diagnostics.
2. Optimization, profiling, fuzzing, and sanitizer integration.
3. Reproducible builds and cross-platform distribution.
4. Compatibility policy, stable documentation, and release validation.

---

## 5. The feature decision record: required before implementation

For every substantial feature, future work should answer these questions in a short design note or commit description:

1. **Problem:** What real programming problem does this solve?
2. **Use cases:** Show at least one realistic VEX example, preferably computational or systems-oriented.
3. **References:** Which established languages or libraries solve a similar problem? Compare at least two approaches when meaningful.
4. **Adoption:** Which ideas are useful to VEX, and which details are intentionally rejected?
5. **Syntax:** What is the smallest readable syntax that fits existing VEX conventions?
6. **Semantics:** What are the type, evaluation, mutation, ownership, error, and lifetime rules?
7. **Representation:** What does the feature become in the typed semantic model and IR?
8. **Backends:** How will C and LLVM implement identical behavior?
9. **Runtime:** Does it require allocation, support code, OS APIs, or external libraries?
10. **Failure behavior:** What happens on invalid input, overflow, allocation failure, or misuse?
11. **Tests:** What are the normal, boundary, negative, and cross-backend cases?
12. **Documentation:** Which specification, matrix, standard-library, architecture, and editor documents need updates?

A feature is complete only when its behavior is implemented and verified end to end. A keyword, AST node, or successful parser test is not enough.

## 6. Rules for keeping the roadmap useful

- The completeness matrix is the implementation-status ledger. Update it with every feature patch.
- This roadmap is the categorized capability map and dependency plan. Update it when priorities or design direction change.
- SPEC.md is the source of truth for accepted syntax and semantics. Do not record undecided proposals there as if they were settled.
- The compiler design review records lessons from external implementations. Add references when research materially influences a decision.
- Keep work directly on the active development branch as currently agreed; do not create feature branches unless that project decision changes.
- Prefer complete vertical slices over scattered syntax patches: parser, semantic checks, IR, verifier, both backends, runtime, tests, and docs.
- Preserve the four-platform CI matrix and the C/LLVM behavior agreement.
- Do not expand into release polish while foundational language capabilities remain the priority.
- Revisit priorities after each milestone based on actual use cases and implementation discoveries.

## 7. Current starting point

At the 2026-11-17 audited baseline, VEX has static primitive types and local inference; functions, control flow, ranges and loop control; nominal structures; fixed one-dimensional arrays plus dynamic and mixed fixed/dynamic jagged nested arrays of primitive and structure values with capacity inspection and reservation; nested aggregate reads, writes, iteration, collection operations, and compound assignment; typed SSA IR with recursive array descriptors and an independent verifier; C11 and LLVM backends; native build/run paths; length-aware byte strings with concatenation and chunk joining; a first-class unsigned `byte`; dynamic `byte[]` buffers; checked owning byte slices; 16-/32-bit endian-aware binary access; whole-file binary read/write; cross-platform CI; and initial editor integration. All-fixed nested arrays and contiguous rectangular multidimensional arrays remain deferred; top-level quoted import syntax is parsed but rejected until module resolution is implemented. The binary-buffer and whole-file I/O milestone passed four-platform CI run [#271](https://github.com/Atiqxyz/vex/actions/runs/43743633949) at commit `e6286e1` (Ubuntu, Windows, macOS ARM, macOS Intel). The [language completeness matrix](language-completeness-matrix.md) is the detailed status authority. The [nested-array design proposal](nested-array-design.md) records the supported dynamic/mixed jagged shapes and the remaining all-fixed size/stack gate.

The binary layer deliberately uses owned byte slices rather than borrowed views: the latter must wait for a sound lifetime and aliasing model. Whole-file I/O opens in binary mode, closes before returning, and reports ordinary I/O failures with `-1`; a general `Result` type, streaming handles, directories, paths, networking, and FFI remain future work. These foundations matter for a capable self-hosted compiler and later graphics, audio, and interactive applications.

Graphics is an explicit long-term target, not an immediate diversion from core compiler work. VEX should eventually support cross-platform windows and input, 2D and 3D rendering, real-time event/frame loops, and scientific visualisation through portable libraries and runtime abstractions. In parallel, every major capability must be evaluated against the self-hosting-capability goal: VEX should eventually be able to express its own lexer, parser, type checker, IR, code generators, tests, and build workflow, even if the compiler remains implemented in C++.

The roadmap is intentionally broad. It is a map for making informed choices, not a checklist that must be blindly completed.