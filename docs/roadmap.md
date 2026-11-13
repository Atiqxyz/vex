# VEX Development Work Plan

**Status:** Proposed roadmap  
**Scope:** Compiler, runtime, standard library, scientific-computing capabilities, and developer ecosystem  
**Guiding goal:** Make VEX a general-purpose systems language that is also genuinely useful for scientific and engineering work.

This roadmap is a plan, not a claim that the listed features already exist. Milestones should be completed through tested, reviewable changes on `dev`, then promoted to `main` only after CI and review pass.

## Product goals

VEX should aim to support:

- Systems programming: predictable performance, explicit control over memory and data layout, interoperability, and low-level operations.
- General application development: files, text, collections, networking, concurrency, testing, and package management.
- Scientific and engineering computing: numerics, complex values, linear algebra, statistics, units, simulation, data processing, and reproducible computation.
- Portability: documented behavior across supported operating systems and architectures.
- A usable ecosystem: good diagnostics, documentation, formatter, package tooling, and stable APIs.

The language should not force every scientific feature into the compiler. Keep the language core relatively small; put most domain functionality in well-designed libraries, with optional specialized packages where appropriate.

## Phase 0 — Design contracts and preserve the foundation

**Purpose:** Prevent the compiler, runtime, and libraries from developing incompatible assumptions.

- Keep `SPEC.md` authoritative for language syntax and semantics.
- Define integer overflow, floating-point behavior, conversions, string representation, and error-handling conventions.
- Finalize the hybrid memory model: ownership, borrowing, lifetimes, allocation, cleanup, and unsafe boundaries.
- Defer module/import visibility, package boundaries, and public API conventions until the core type system and standard-library surface are mature enough to organize.
- Define target platforms, ABI strategy, and C interoperability goals.
- Establish compatibility and deprecation policies for the standard library.
- Maintain a clear distinction between implemented, planned, and experimental features.

**Exit criteria:** Design decisions are documented, unresolved questions are explicit, and compiler/runtime interfaces have stable initial contracts.

## Phase 1 — Typed intermediate representation (IR)

**Purpose:** Give VEX a backend-independent representation of validated programs.

- Lower the typed AST into typed IR.
- Represent functions, parameters, values, basic blocks, control flow, calls, returns, and source locations.
- Choose and document the SSA/control-flow merge representation.
- Add an IR verifier for types, definitions, uses, dominance or equivalent ordering rules, block targets, and terminators.
- Preserve source spans so backend and IR diagnostics can refer to VEX source.
- Add readable IR dumps and focused unit tests.

**Exit criteria:** Supported source programs lower deterministically into valid IR; malformed IR is rejected by the verifier.

## Phase 2 — First executable backend

**Purpose:** Make simple VEX programs run end to end.

- Implement the first backend; evaluate portable C generation as the bootstrap route.
- Define a stable compiler pipeline: source → lexer/parser → semantic analysis → IR → backend → executable.
- Support the currently implemented language subset, including functions, primitive values, expressions, conditionals, repetition, and returns.
- Add linking and clear compiler diagnostics.
- Test generated programs on Linux, Windows, and macOS where CI supports them.
- Keep backend-specific details out of the frontend and IR.

**Exit criteria:** Small VEX programs compile, link, execute, and produce expected results on supported CI platforms.

## Phase 3 — Runtime foundation

**Purpose:** Supply the behavior that generated programs rely on.

- Implement program entry and exit, process arguments, and exit codes.
- Implement the runtime side of `say` and basic input/output.
- Establish string storage, UTF-8 behavior, allocation, and cleanup.
- Implement panic/failure reporting and the chosen error conventions.
- Add foundational memory utilities and safe/unsafe boundaries.
- Add runtime tests, including allocation and cleanup stress tests.

**Exit criteria:** Runtime behavior is documented, tested, and consistent across supported targets.

## Phase 4 — Core language types and data structures

**Purpose:** Give programmers expressive, safe, efficient building blocks.

### Foundational types

- Fixed-width signed and unsigned integers (for example, 8/16/32/64-bit, with wider integers where practical).
- Floating-point types with clearly specified IEEE-754 behavior (at least 32-bit and 64-bit).
- Boolean, character/code-point handling, UTF-8 strings, and byte buffers.
- Enumerations, structures, tuples, and tagged unions.
- Option/Maybe and Result-style types for absence and recoverable errors.
- Arrays, slices/views, vectors, and maps/sets.
- References, pointers, spans, and atomics consistent with the memory model.
- Generic types and generic functions when the type-system design is ready.

### Core operations

- Checked and wrapping arithmetic where explicitly requested.
- Numeric conversions with clear checked/trapping behavior.
- Comparisons, hashing, iteration, sorting, searching, and formatting.
- Explicitly documented value/reference semantics and data layout.

**Exit criteria:** Core types have stable semantics, useful diagnostics, comprehensive tests, and documented performance characteristics.

## Phase 5 — Standard library v1

**Purpose:** Make ordinary application and systems programming practical.

| Module | Planned capabilities |
|---|---|
| `core` | Fundamental types, conversions, assertions, comparison, option/result, common traits or interfaces |
| `io` | Standard input/output/error, buffered I/O, readers/writers, formatting |
| `string` | UTF-8 strings, builders, search, split, join, trim, replace, parsing |
| `collections` | Dynamic arrays/vectors, slices, maps, sets, queues, stacks, heaps |
| `math` | Numeric functions, powers, roots, trigonometry, logarithms, exponentials, constants |
| `random` | Seedable generators, reproducible streams, common probability distributions |
| `filesystem` | Paths, files, directories, metadata, buffered file operations |
| `time` | Monotonic clocks, durations, timestamps, calendar/date utilities |
| `format` | Consistent text, numeric, and diagnostic formatting |
| `process` | Arguments, environment variables, process execution, exit status |
| `testing` | Assertions, test discovery, fixtures, benchmarks, test reports |
| `sync` | Mutexes, condition variables, channels, atomics, once initialization |

**Exit criteria:** A programmer can build useful command-line tools and ordinary applications without needing ad hoc platform code for common tasks.

## Phase 6 — Scientific and engineering foundations

**Purpose:** Make VEX particularly capable for researchers, scientists, and engineers.

### Numeric and mathematical types

- Complex numbers with well-defined arithmetic and common functions.
- Arbitrary-precision integers and, where feasible, arbitrary-precision decimal or floating-point support through a library.
- Rational numbers for exact fractions.
- Fixed-point and decimal types for domains where binary floating point is unsuitable.
- Numeric ranges/intervals with explicit endpoint and rounding semantics.
- Vectors and matrices, with shape and layout clearly represented.
- N-dimensional arrays/tensors, including shape, strides, and views.
- Units and physical quantities that prevent invalid dimensional operations.
- Physical constants with units and documented sources/precision.
- Optional SIMD/vector types for performance-sensitive workloads.

### Scientific library modules

| Module | Planned capabilities |
|---|---|
| `linalg` | Vector/matrix operations, decompositions, solving linear systems, eigenvalues, least squares |
| `fft` | Fourier transforms and related spectral operations |
| `stats` | Descriptive statistics, distributions, sampling, regression, correlation |
| `numerical` | Root finding, interpolation, numerical differentiation/integration, stable summation |
| `ode` | Ordinary differential equation solvers, adaptive step sizes, event handling |
| `optimize` | Minimization, maximization, constrained optimization, fitting |
| `units` | Dimensional analysis, unit conversion, SI and commonly used derived units |
| `constants` | Mathematical and physical constants with provenance and precision |
| `signal` | Filtering, convolution, transforms, signal-processing utilities |
| `geometry` | Coordinate systems, rotations, transformations, geometric primitives |
| `data` | Tabular data, CSV and structured-data readers, efficient data transformations |

### Domain examples and packages

Build examples and optional packages for:

- Physics: mechanics, electromagnetism, thermodynamics, quantum-related numerical tools.
- Aerospace: orbital mechanics, coordinate frames, attitude representations, trajectory propagation, propulsion calculations, and simulation utilities.
- Chemistry: unit-aware quantities, molecular data structures, numerical chemistry utilities, and scientific data handling.
- Engineering: control systems, signal processing, numerical solvers, and parameter estimation.
- Computational research: Monte Carlo methods, reproducible random experiments, simulation, and data analysis.

These are library and ecosystem goals, not promises that every domain solver belongs in the language's core distribution.

**Scientific correctness requirements:**

- Document numerical stability, precision, error bounds, and algorithm limitations.
- Test against trusted reference datasets and independently verified results.
- Include edge cases: overflow, underflow, NaN, infinities, singular matrices, ill-conditioned systems, and convergence failures.
- Make random seeds and computation settings reproducible.
- Record units and conventions explicitly; never silently mix incompatible dimensions.
- Benchmark representative workloads and avoid claiming performance without measurements.

**Exit criteria:** VEX can express and run representative scientific workloads with tested numerical behavior and useful documentation.

## Phase 7 — Systems and connectivity libraries

**Purpose:** Expand VEX into a capable systems and backend language.

- Networking: TCP/UDP sockets, DNS, TLS integration, and HTTP client/server APIs.
- Concurrency: threads, tasks, synchronization, channels, and cancellation.
- Memory: allocators, arenas, pools, aligned allocation, and profiling tools.
- Interoperability: C ABI, foreign-function interface, and bindings to existing libraries.
- Binary data: byte order, serialization, parsing, and efficient buffers.
- Compression and cryptography through reviewed implementations or maintained dependencies.
- Operating-system APIs behind portable abstractions where appropriate.

**Exit criteria:** VEX can build practical networked and concurrent systems while retaining controlled access to platform-specific functionality.

## Phase 8 — Tooling and package ecosystem

**Purpose:** Make VEX pleasant to use and maintain.

- Package manager with dependency resolution, lockfiles, versioning, and reproducible builds.
- Formatter and linter.
- Documentation generator and searchable standard-library documentation.
- Language server support: diagnostics, completion, navigation, formatting, and refactoring.
- Debug information and debugger integration.
- Profiler and benchmark tooling.
- Cross-compilation and target configuration.
- Stable package publishing and security/update guidance.

**Exit criteria:** New users can install VEX, create a project, add dependencies, build, test, debug, and document it using supported tools.

## Phase 9 — Optimization and advanced execution

**Purpose:** Improve performance without sacrificing correctness.

- IR optimization passes: constant folding, dead-code elimination, inlining where beneficial, and common subexpression elimination.
- Escape analysis and allocation improvements where supported by the memory model.
- Profile-guided optimization when the toolchain supports it.
- Native machine-code backend evaluation and implementation.
- SIMD and hardware-specific acceleration behind portable interfaces.
- GPU compute as a later, separately scoped capability.
- Performance regression benchmarks across representative workloads.

**Exit criteria:** Optimizations preserve specified behavior and show measured improvements on repeatable benchmarks.

## Phase 10 — Release, compatibility, and self-hosting research

- Define VEX v0.1 scope and explicitly list unsupported features.
- Publish language, runtime, and standard-library documentation.
- Maintain conformance tests and cross-platform CI.
- Establish semantic-versioning and compatibility policies as the project matures.
- Build real projects as acceptance tests: CLI tool, file processor, numerical simulation, and networked application.
- Explore self-hosting only after the language, runtime, and tooling are mature enough to make it a useful engineering goal.

## Recommended repository organization (future target)

```text
vex/
├── compiler/
│   ├── frontend/
│   ├── semantic/
│   ├── ir/
│   └── backends/
├── runtime/
├── stdlib/
│   ├── core/
│   ├── io/
│   ├── string/
│   ├── collections/
│   ├── math/
│   ├── filesystem/
│   └── ...
├── packages/
│   └── scientific/
│       ├── linalg/
│       ├── stats/
│       ├── numerical/
│       ├── units/
│       └── ...
├── tests/
├── examples/
│   ├── systems/
│   ├── scientific/
│   └── engineering/
└── docs/
```

This is a conceptual target layout. Adopt it incrementally when the code justifies the separation; do not reorganize the repository merely to match a diagram.

## Engineering rules for every milestone

1. Work on `dev`; do not push unverified work directly to `main`.
2. Every feature needs tests, documentation, and a clear definition of done.
3. Run the supported OS CI matrix and investigate failures before promotion.
4. Keep public APIs small and consistent; avoid adding duplicate ways to do the same thing without a reason.
5. Separate stable standard-library modules from experimental domain packages.
6. Prefer well-reviewed dependencies for complex algorithms and security-sensitive primitives over rushed home-grown implementations.
7. Measure performance, numerical accuracy, and memory use instead of relying on assumptions.
8. Mark each item as planned, in progress, implemented, or deferred. Never describe a planned feature as already available.

## Immediate next steps

1. Broaden the executable language core: ranges, complete iteration semantics, and the control-flow features now being added.
2. Design the type and memory contracts before adding arrays, slices, structs, enums, and pointers.
3. Add foundational library capabilities for numerics, text, bytes, files, random numbers, and time.
4. Add error handling and deterministic resource cleanup.
5. Design modules/imports after enough stable language and library functionality exists to organize.
6. Add scientific foundations in layers, beginning with complex numbers, unit-aware quantities, vectors/matrices, statistics, and numerical methods.
7. Validate the language with end-to-end computational programs and cross-platform tests.

The roadmap is intentionally ambitious. It is a long-term direction, not a requirement to implement every module before the first usable VEX release.
