# VEX language feature survey and adoption plan

This document records the cross-language survey behind VEX's planned expansion. It is a capability map, not a promise that every item is already implemented. SPEC.md remains authoritative for VEX syntax and semantics.

## Design rules

1. **Adopt capabilities, not keyword collections.** If several languages solve the same problem, VEX should choose one coherent construct instead of preserving multiple spellings for the same operation.
2. **Make common behavior easy.** Infer types and choose safe defaults when one answer is unsurprising.
3. **Keep important control configurable.** Allocation strategy, numeric widths, overflow behavior, concurrency policy, and target settings must remain expressible.
4. **Do not make grammar do the standard library's job.** Math, filesystem, text, and collection operations should generally be typed library functions, not special syntax.
5. **Only promise features end to end.** Syntax needs semantics, IR support, backend behavior, tests, and documentation before it is called implemented.

## Languages surveyed

| Language | Useful capabilities considered | VEX design response |
|---|---|---|
| Python | Readable compound statements, loops, iteration, comprehensions, context-managed resources, exceptions, rich built-ins | Keep VEX's braces and static typing; adopt clear iteration and resource-management ideas. Put general operations in typed libraries. |
| C | Fixed-width data access, bitwise operations, pointers, explicit layout, predictable low-level behavior, C interoperability | Preserve low-level access as an advanced capability, but do not require manual allocation for ordinary values. |
| C++ | RAII, deterministic destruction, templates, value semantics, standard algorithms, operator customization | Study RAII and generic programming. Avoid forcing complex templates or operator overloading into normal code. |
| Rust | Enums, exhaustive pattern matching, ownership/borrowing, Option/Result, closures, traits | Design enums and matching together; specify lifetime safety and explicit result handling before pointers or closures. |
| Go | Range iteration, interfaces, defer, goroutines/channels, straightforward error values | Prefer one clean iteration model, consider deterministic deferred cleanup, and design concurrency around explicit runtime primitives. |
| Zig | Allocator values, defer/errdefer, optionals, error unions, compile-time execution, explicit numeric behavior | Preserve low-level choices for systems code; offer ordinary defaults where safe and predictable. |
| Julia | Multiple dispatch, parametric types, ranges, comprehensions, broadcasting, complex numbers, numerical computing | Study numerical ergonomics and generic math. Keep domain-specific numerical tools in libraries. |
| Fortran | Multidimensional arrays, array sections, elemental operations, numerical-oriented intrinsic functions | Prioritize efficient arrays, slices/views, predictable layout, and scientific math facilities. |
| Swift | Optionals, value types, pattern matching, structured concurrency, automatic reference counting | Consider explicit optional types and structured concurrency; do not conflate reference counting with the entire VEX memory model. |

The purpose is to borrow proven capabilities, not clone any language or copy its entire standard library.

## Planned VEX capability map

### 1. Declarations and functions

- Mutable and immutable bindings with inference and optional explicit type annotations.
- Typed function parameters and return types; recursion and forward calls.
- Optional explicit make main() entry point. Without it, top-level statements execute in source order through a compiler-generated entry function. With it, top-level executable statements are rejected to avoid two competing execution paths.
- Default argument values and named arguments if they do not create overload or call-resolution ambiguity.
- Generic functions and types after type identity and constraints are designed.
- Closures and function values after capture and lifetime behavior can be specified.
- Variadic functions only where a safe, typed use case exists.

### 2. Control flow

- if / else conditional branching.
- Counted repeat N times and condition-driven while loops.
- break and continue targeting the innermost enclosing loop.
- One `for-in` construct for fixed arrays and integer ranges is implemented; strings and other iterable values remain planned once their collection semantics exist.
- First-class range values with clearly documented inclusive/exclusive endpoints.
- match with exhaustive patterns after enums and tagged unions exist.
- Consider switch-like syntax only if a concrete low-level use case cannot be expressed cleanly by match.
- Avoid duplicate do-while, until, loop, or alternate counted-loop spellings without a real semantic need.
- Consider defer-like deterministic cleanup after behavior on return, break, continue, errors, and nested scopes is specified.

### 3. Expressions and operators

- Arithmetic, comparisons, Boolean logic, unary operators, grouping, function calls, and assignment.
- Compound assignment (+=, -=, *=, /=, %=) as shorthand over existing operations, not as a separate arithmetic implementation.
- Bitwise AND/OR/XOR/NOT and shifts for fixed-width integer work.
- Explicit casts and conversions; checked conversion where narrowing can fail.
- Indexing, slicing, field access, and range expressions after arrays, slices, and structures are specified.
- Short-circuit Boolean evaluation.
- No automatic ++/-- addition merely to duplicate += 1 and -= 1.
- Defer operator overloading until value semantics and generic constraints are stable.

### 4. Data and type system

- Fixed-width signed and unsigned integers, plus 32-bit and 64-bit floating-point types.
- Character/code-point handling, UTF-8 strings, and raw byte buffers as distinct concepts.
- Fixed arrays, dynamic arrays/vectors, slices/views, tuples, structures, maps, and sets.
- Enums and tagged unions with exhaustive pattern matching.
- Optional values and Result-style values for absence and recoverable errors.
- Generics and interfaces/traits after type identity, bounds, and dispatch rules are stable.
- Complex numbers, rational values, ranges, and unit-aware quantities for scientific work, preferably as library or standard types unless compiler support is necessary.

### 5. Memory and resource management

- Automatic storage and cleanup for ordinary local values where lifetime rules permit it.
- Deterministic destruction rather than surprising collection pauses for systems workloads.
- Explicit allocator selection, arenas, pools, aligned allocation, and manual allocation for cases that need configuration.
- References and pointers with documented safety boundaries.
- A defer-like cleanup facility for files, locks, buffers, and other resources after all control-flow exit cases are specified.
- No malloc-style ceremony for normal arrays or strings; no removal of low-level allocation control.
- Do not promise garbage collection, reference counting, or ownership rules until their costs and edge cases are evaluated.

### 6. Errors, compile-time features, and runtime

- Explicit optional and Result/error value types.
- Concise error propagation once result types and function return rules are settled.
- Assertions, panic/trap behavior, and recoverable errors with distinct semantics.
- Compile-time evaluation for constants, generic code, and carefully bounded metaprogramming.
- Process arguments, environment access, exit codes, and runtime diagnostics.
- Threads, tasks, channels, synchronization, atomics, and structured concurrency after runtime and memory semantics exist.
- Exceptions are not selected as the default error model; this remains an open design decision.

### 7. Standard-library capabilities

| Area | Capabilities to provide |
|---|---|
| Math | Roots, powers, trigonometry, inverse trigonometry, logarithms, exponentials, rounding, constants, numeric utilities |
| Numeric | Complex numbers, arbitrary precision through libraries, rational/fixed-point types, checked conversions |
| Collections | Dynamic arrays, slices, maps, sets, queues, stacks, heaps, sorting, searching, hashing |
| Text | UTF-8 operations, builders, formatting, parsing, search, split, join, trim, replace |
| Bytes | Byte buffers, endian conversion, binary readers/writers, encoding and decoding |
| I/O | Standard input/output/error, buffered I/O, readers and writers |
| Filesystem | Paths, files, directories, metadata, buffered file operations |
| Random | Seedable generators, reproducible streams, sampling, probability distributions |
| Time | Monotonic clocks, durations, timestamps, calendar utilities |
| Process | Arguments, environment variables, process creation, exit status |
| Testing | Assertions, test discovery, fixtures, benchmarks, structured reports |
| Concurrency | Threads, tasks, channels, mutexes, condition variables, atomics |
| Networking | TCP/UDP, DNS, TLS integration, HTTP client/server APIs |
| Serialization | CSV, JSON, binary formats, efficient parsing |

Scientific and engineering packages can then provide linear algebra, statistics, numerical solvers, units, geometry, orbital mechanics, simulation, and scientific data processing without bloating the grammar.

### 8. Modules and imports

Modules/imports intentionally come later. Their visibility, initialization, package boundaries, dependency model, and public API rules should be designed once core types and standard-library APIs are stable enough to organize.

## Current implementation slice

The current `dev` branch includes:

- optional explicit main(); absent main, top-level statements run in source order through a compiler-generated entry function;
- while condition { ... };
- break and continue in while and counted repeat loops;
- Boolean checking for while conditions and diagnostics when loop controls are used outside loops;
- compound assignment operators +=, -=, *=, /=, and %=;
- compound assignment desugaring through the existing assignment and arithmetic semantic model;
- SSA control-flow lowering for while and loop-control edges;
- inferred one-dimensional fixed arrays with primitive elements;
- half-open and inclusive integer ranges in `for-in` loops, lowered directly to SSA control flow;
- array literals, checked indexing, and indexed assignment;
- fixed-array and integer-range `for-in` iteration lowered through explicit SSA control flow;
- C struct and LLVM aggregate representations for fixed arrays.

With explicit main(), top-level executable statements are rejected; top-level values in implicit-entry mode are not captured by ordinary functions. Fixed arrays are value aggregates and do not require heap allocation. Dynamic arrays, slices, structs, enums, pattern matching, generics, result values, memory controls, and bitwise operators remain planned. The integer-range iteration implementation passed the four-platform CI run recorded during its implementation; it remains an experimental development-branch feature until a release includes it.

## Primary language references

- Python Language Reference: https://docs.python.org/3/reference/
- Python compound statements: https://docs.python.org/3/reference/compound_stmts.html
- C language reference: https://en.cppreference.com/w/c
- C++ language reference: https://en.cppreference.com/w/cpp/language
- Rust Reference: https://doc.rust-lang.org/reference/
- Rust patterns: https://doc.rust-lang.org/reference/patterns.html
- Go language specification: https://go.dev/ref/spec
- Zig language reference: https://ziglang.org/documentation/
- Julia manual: https://docs.julialang.org/en/v1/manual/
- Fortran learning resources: https://fortran-lang.org/learn/
- Swift language guide: https://docs.swift.org/swift-book/documentation/the-swift-programming-language/
