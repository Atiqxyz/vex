# VEX language feature survey and adoption plan

This document records the cross-language survey behind VEX's planned expansion. It is a selection guide, not a promise that every listed feature is implemented. The authoritative syntax and semantic contract remains SPEC.md.

## Design rules

1. Adopt capabilities, not keyword collections. When two languages solve the same problem, VEX should choose one coherent construct rather than preserve multiple spellings for equivalent behavior.
2. Make common behavior easy. Infer types and choose safe defaults when there is one unsurprising answer.
3. Keep important control configurable. Allocation strategy, numeric widths, overflow behavior, concurrency policy, and target settings must remain expressible when they matter.
4. Do not make grammar do the standard library's job. Mathematical, filesystem, text, and collection operations should generally be library functions, not special syntax.
5. Only promise features end to end. A construct is not supported merely because the parser accepts it; it needs semantic rules, IR representation, backend behavior, tests, and documentation.

## Surveyed language ideas

| Source language | Useful ideas considered | VEX design response |
|---|---|---|
| Python | Readable compound statements, iteration, comprehensions, context-managed resources, exceptions | Keep brace-delimited blocks and newline separation; adopt clear iteration and resource-management concepts without copying indentation-based suites or dynamic typing |
| C | Fixed-width data access, bitwise operations, pointers, explicit layout, predictable low-level behavior | Preserve low-level control as an opt-in capability, but avoid requiring manual allocation for ordinary values |
| Rust | Algebraic data types, exhaustive pattern matching, ownership/borrowing, Option/Result, closures | Plan enums and pattern matching together; design safe lifetimes and explicit result handling before exposing low-level pointers |
| Go | Straightforward range iteration, interfaces, deferred cleanup, concurrency primitives | Prefer one straightforward iteration model over several overlapping loop syntaxes; consider interfaces and deferred cleanup after type and lifetime rules are stable |
| Zig | Explicit optionals and error unions, defer, compile-time execution, allocator choice, explicit overflow behavior | Keep configuration available for systems work; define checked/wrapping arithmetic and allocator contracts rather than silently guessing |

## Planned VEX capability map

### 1. Declarations and functions

- Mutable and immutable local bindings with inference and optional explicit annotations.
- Functions with typed parameters and return types, and forward function calls.
- Optional explicit make main() entry point; the intended fallback is top-level execution when main is absent. The fallback is a design target and is not yet implemented.
- Generic functions and types after the type model is designed.
- Closures and function values when capture and lifetime behavior can be specified safely.
- Default parameter values only if they do not create overload-like ambiguity.

### 2. Control flow

- if / else conditional branching.
- Counted repeat N times.
- Condition-driven while.
- break and continue, targeting the innermost enclosing loop.
- One for-in construct for ranges and iterable values once those types exist.
- Range values with explicitly documented inclusive/exclusive endpoints.
- match with exhaustive patterns after enums and tagged unions exist.
- Avoid adding do-while, until, a second loop spelling, or multiple equivalent counted-loop forms unless a real semantic need appears.

### 3. Expressions and operators

- Arithmetic, comparisons, boolean logic, unary operators, grouping, and calls.
- Compound assignment (+=, -=, *=, /=, %=) as shorthand over existing operations, not as a separate arithmetic engine.
- Bitwise operations and shifts for low-level and numeric work once fixed-width integer types exist.
- Explicit casts/conversions with checked behavior where narrowing can fail.
- Indexing, slicing, field access, and range expressions after arrays, slices, and structures are specified.
- No automatic insertion of ++/-- merely to duplicate += 1 / -= 1.

### 4. Data and type system

- Fixed-width signed and unsigned integers; 32-bit and 64-bit floating-point types.
- Characters/code points, UTF-8 strings, and raw byte buffers as clearly distinct concepts.
- Fixed arrays, dynamic arrays/vectors, slices/views, tuples, structures, maps, and sets.
- Enums and tagged unions with exhaustive pattern matching.
- Option/optional values and Result/error values for explicit absence and recoverable failure.
- Generics and interfaces/traits once type identity, constraints, and dispatch rules are settled.

### 5. Memory and resource management

- Automatic storage and cleanup for ordinary local values where lifetime rules permit it.
- Deterministic cleanup rather than surprising hidden allocations or collection pauses for systems workloads.
- Explicit allocator selection, arenas, pools, aligned allocation, and manual allocation for cases that need configuration.
- References and pointers with explicit safety boundaries.
- A defer-like cleanup construct after return, break, continue, panic, and nested-scope behavior is specified.
- No raw malloc-style ceremony for normal arrays or strings; no removal of low-level allocation control.

### 6. Errors, resources, and concurrency

- Explicit Option and Result/error-union types.
- A concise error-propagation construct once error values and function return semantics are fixed.
- Deterministic resource cleanup for files, locks, and other acquired resources.
- Threads, tasks, async/await, channels, synchronization, and atomics after runtime and memory semantics exist.
- Exceptions are not selected as the default error model; this remains a design decision to be finalized.

### 7. Standard-library functions

- Expand math as a coherent library: roots, powers, trigonometry, inverse trigonometry, logarithms, exponentials, rounding, constants, and numeric utilities.
- Add text, byte, collection, filesystem, I/O, random, time, formatting, process, and testing libraries.
- Avoid language-specific duplicate built-ins when an ordinary typed library function is sufficient.
- Scientific packages should provide linear algebra, statistics, numerical solvers, units, geometry, and simulation utilities without bloating the grammar.

### 8. Modules and imports

Modules/imports remain intentionally later. Their visibility, initialization, package boundaries, and dependency model should be designed only after the core types and standard-library APIs are established.

## Current implementation slice

The feature/core-language-expansion branch adds:

- while condition { ... };
- break and continue in while and counted repeat loops;
- Boolean checking for while conditions and diagnostics when loop controls are used outside loops;
- compound assignment operators +=, -=, *=, /=, and %=, lowered through the existing arithmetic and assignment semantics.

These changes still require CI and end-to-end validation before being considered release-ready. for-in remains parser-only until iterable types exist. Arrays, structs, enums, pattern matching, generics, error values, memory controls, and implicit main remain planned work.

## References

- Python language reference: https://docs.python.org/3/reference/compound_stmts.html
- Rust Reference, patterns: https://doc.rust-lang.org/reference/patterns.html
- Rust Reference, expressions: https://doc.rust-lang.org/reference/expressions.html
- Go language specification: https://go.dev/ref/spec
- Zig language reference: https://ziglang.org/documentation/
