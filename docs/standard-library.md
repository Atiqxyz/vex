# VEX Standard Library

The VEX standard library is being introduced incrementally. The initial implementation provides a small mathematical surface as compiler-recognized built-ins. These functions are type-checked by the frontend, lowered to dedicated math IR instructions, verified independently, and emitted as calls to the platform C math library by the current C11 bootstrap backend.

## Initial mathematical functions

All arguments and return values use VEX `float` (IEEE-754 double precision in the current backend).

| Function | Signature | Description |
|---|---|---|
| `sqrt(x)` | `float -> float` | Square root |
| `sin(x)` | `float -> float` | Sine; input is in radians |
| `cos(x)` | `float -> float` | Cosine; input is in radians |
| `pow(x, y)` | `(float, float) -> float` | Raises x to the power y |
| `abs(x)` | `float -> float` | Absolute value |

An integer argument can be widened to float where a function expects float.

## Example

```vex
make main() {
    let radius = 12.0
    let area_factor = pow(radius, 2.0)
    let root = sqrt(area_factor)

    say(root)
    say(sin(0.0))
    say(cos(0.0))
    say(abs(0.0 - 5.0))
}
```

## Current implementation notes

- These functions are compiler-known built-ins, not yet separately compiled importable modules.
- The backend uses the host C math library. The toolchain links `libm` on Unix-like systems; Windows math functions are provided by the C runtime.
- Floating-point domain errors and exceptional values follow the platform C math library behavior.
- This is the first slice of the standard library, not a complete scientific-computing API.

## String byte length (implemented)

`length(value)` also accepts a `string` and returns its byte count. VEX strings are length-aware byte sequences; this operation does not count Unicode scalar values or grapheme clusters, validate UTF-8, or stop at embedded NUL bytes. For example, `length("A\\0B")` is `3`.

## Unsigned byte values (implemented)

VEX provides an unsigned `byte` scalar with values from 0 through 255. An `int` does not implicitly narrow to `byte`. Use `to_byte(value)` for an explicit checked conversion; it traps when the input is negative or greater than 255. `to_int(value)` explicitly widens a byte to a signed 64-bit integer without changing its non-negative value.

Byte values support equality, unsigned relational comparisons, and byte-width bitwise operations (`&`, `|`, `^`, and `~`). These operations require matching operand types; byte operations return a byte and `~` flips exactly eight bits. Bytes are not currently general arithmetic operands. `say(byte)` prints its unsigned decimal value.

Bytes work with the existing aggregate model: `byte[N]` fixed arrays, `byte[]` dynamic arrays, structure fields, function parameters and return values, indexing, iteration, and `push`/`pop`/`reserve`. Dynamic arrays retain the same shared-reference and program-lifetime allocation behavior as other element types.

This provides typed in-memory binary data. Dedicated file/network I/O, byte slices, and endian-aware integer serialization helpers are not yet implemented.

## Dynamic-array built-ins (implemented)

Dynamic arrays hold primitive or nominal structure values and are declared with an empty length suffix, for example `int[]`, `byte[]`, `string[]`, or `Point[]`. These compiler-recognized operations are available:

| Function | Signature | Behavior |
|---|---|---|
| `length(value)` | `string -> int`; `T[] -> int` (also accepts fixed arrays) | Returns a string's byte count or an array's element count; fixed-array length is emitted as a constant |
| `push(array, value)` | `(T[], T) -> void` | Appends an element, growing capacity geometrically when needed |
| `pop(array)` | `T[] -> T` | Removes and returns the final element; traps when the array is empty |
| `capacity(array)` | `T[] -> int` | Returns the number of allocated element slots |
| `reserve(array, n)` | `(T[], int) -> void` | Ensures capacity is at least `n` elements without changing length or contents; never shrinks |

Dynamic arrays are shared heap references. Assignments and function calls copy the reference, so mutation is visible through aliases. The runtime tracks allocations and frees them when the generated entry function returns. Fixed arrays remain value aggregates. Dynamic arrays support primitive and nominal structure elements; nested arrays, slices, and per-scope deallocation are not implemented. Use `reserve` when the expected element count is known to avoid geometric growth reallocations; `capacity` reports allocated element slots.

```vex
make main() {
    let values: int[] = []
    reserve(values, 2)
    push(values, 10)
    push(values, 20)

    say(length(values))
    say(pop(values))
    say(length(values))
}
```

## Planned expansion

Future library work can add constants, additional trigonometric and transcendental functions, numeric utilities, string operations, file I/O, and time/system facilities. New functions should be added with explicit signatures, IR verification rules, backend behavior, documentation, and executable integration tests.

## Planned mathematical expansion (not implemented)

The following is a deduplicated capability inventory for the future math library. These names are proposals, not currently callable VEX functions. Add them as ordinary typed library functions where possible, not as new grammar keywords.

| Group | Candidate functions | Purpose |
|---|---|---|
| Basic numeric | min, max, clamp, sign, abs | Common scalar operations; preserve input numeric type where the type system allows it |
| Rounding | floor, ceil, trunc, round | Explicit rounding modes rather than implicit changes to ordinary arithmetic |
| Roots and powers | sqrt, cbrt, pow, hypot | Roots, exponentiation, and stable hypotenuse calculation |
| Exponentials and logarithms | exp, exp2, log, log2, log10 | Natural and base-specific exponential/logarithmic operations |
| Trigonometry | sin, cos, tan, asin, acos, atan, atan2 | Circular functions and inverse functions, with radians as the default angle unit |
| Hyperbolic | sinh, cosh, tanh, asinh, acosh, atanh | Hyperbolic and inverse hyperbolic functions |
| Floating-point utilities | is_nan, is_finite, is_infinite, copysign, fma | Classification and precision-sensitive operations |
| Integer utilities | gcd, lcm | Number-theoretic helpers; bit counts belong with integer/bit operations |

Mathematical constants should be provided as typed constants rather than zero-argument functions. Candidates include pi, tau, e, sqrt2, positive infinity, and NaN, with exact names and availability to be specified.

### Mathematical API rules

- Use radians for trigonometric functions and document that convention.
- Keep random-number generation in a separate random library with explicit, reproducible seeds; do not mix stateful random behavior into math.
- Keep vector/matrix, tensor, statistics, numerical integration, and differential-equation solvers in dedicated scientific libraries.
- Define behavior for NaN, infinities, signed zero, domain errors, and overflow consistently across C and LLVM backends.
- Make integer and floating-point variants preserve useful type information. Do not silently convert every numeric utility to floating point.
- Where functions share semantics, prefer one generic or overloaded operation only after VEX's generic/type-resolution rules support it cleanly.

## Planned non-mathematical API inventory (not implemented)

This inventory gathers common, useful operations seen across established language libraries. It deliberately describes capabilities rather than copying every language's names. Exact naming, types, overloads, error behavior, and namespace boundaries should be decided together when VEX has collections, strings, and modules. Equivalent names must not become duplicate operations.

| Area | Candidate API capabilities |
|---|---|
| Core values | type_name, compare, swap, assert, panic, checked conversion, formatting |
| Strings | length, is_empty, contains, starts_with, ends_with, find, replace, split, join, trim, trim_start, trim_end, lower, upper, parse_int, parse_float |
| Byte-buffer utilities | slice, read/write integer, read/write float, endian conversion, encode/decode |
| Dynamic arrays | capacity, is_empty, reserve, resize, insert, remove, clear, sort, reverse, contains, find |
| Maps and sets | insert, get, get_or_default, contains, remove, keys, values, entries, union, intersection, difference |
| Algorithms | sort, stable_sort, binary_search, linear_search, min_element, max_element, accumulate, transform, filter, reduce |
| Files and paths | open, create, read, write, append, flush, close, exists, metadata, list_directory, join_path, normalize_path |
| Input/output | read_line, write, write_line, read_bytes, write_bytes, buffered_reader, buffered_writer |
| Random | generator_create, seed, next_int, next_float, choose, shuffle, sample, uniform, normal |
| Time | now, monotonic_now, sleep, elapsed, add_duration, parse_timestamp, format_timestamp |
| Process | arguments, environment, current_directory, exit, spawn, wait, status |
| Testing | test, assert_equal, assert_approx, assert_raises, benchmark, measure_time |

### API design constraints

- Prefer one canonical name and one well-defined behavior for each operation; avoid aliases such as `len` and `length` unless there is a clear interoperability reason.
- Prefer methods or free functions consistently once VEX's receiver, generic, and namespace conventions have been designed; do not mix styles arbitrarily.
- Collection APIs must define ownership, mutation, capacity growth, iterator invalidation, and allocator behavior.
- String APIs must distinguish UTF-8 text from raw bytes and define indexing semantics (bytes, code points, or grapheme clusters).
- File and process operations must return explicit errors and close resources deterministically.
- Random APIs must make seeding and reproducibility explicit.
- Time APIs must distinguish monotonic durations from wall-clock timestamps.
- Algorithms should compose with common iterable and slice types rather than introduce parallel APIs for each container.
- Keep advanced capabilities configurable without making ordinary calls verbose: provide safe defaults and optional configuration objects/arguments for allocator, buffering, capacity, and precision choices.

## Byte-preserving string joining

```vex
let chunks: string[] = ["VEX", " ", "language"]
let message = join(chunks)
say(message)
```

The built-in `join(chunks)` accepts a dynamic `string[]` and returns one immutable string containing the chunks in order. It preserves embedded NUL bytes and exact byte lengths. The runtime first checks the total length for overflow, then allocates the final string once and copies each non-empty chunk once. Joining an empty array, or an array containing only empty strings, returns an empty string without allocating.

A reusable chunk-based builder can be written directly in VEX:

```vex
struct StringBuilder {
    chunks: string[],
}

make builder_append(builder: StringBuilder, chunk: string) {
    push(builder.chunks, chunk)
}

make builder_build(builder: StringBuilder) -> string {
    return join(builder.chunks)
}

make main() {
    let builder = StringBuilder { chunks: [] }
    builder_append(builder, "VEX")
    builder_append(builder, " runtime")
    let snapshot = builder_build(builder)
    builder_append(builder, " builder")
    say(snapshot)
    say(builder_build(builder))
}
```

The builder stores string descriptors in VEX's existing geometrically growing dynamic array rather than repeatedly concatenating the accumulated text. Appending a chunk is amortized O(1) in the number of chunks; building the final string is O(number of chunks + total output bytes), with one final string allocation for non-empty output. `builder_build` creates an immutable snapshot, so later appends do not change previously built strings. Dynamic-array storage and joined-string allocations follow the current program-lifetime cleanup model and are released when the generated entry function returns normally.

This is a chunk builder: it is best when text arrives in pieces. If each chunk is itself produced by repeated string concatenation, that chunk's own construction can still be quadratic.