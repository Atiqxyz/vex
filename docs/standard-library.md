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
