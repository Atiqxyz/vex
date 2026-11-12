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
    say(abs(-5.0))
}
```

## Current implementation notes

- These functions are compiler-known built-ins, not yet separately compiled importable modules.
- The backend uses the host C math library. The toolchain links `libm` on Unix-like systems; Windows math functions are provided by the C runtime.
- Floating-point domain errors and exceptional values follow the platform C math library behavior.
- This is the first slice of the standard library, not a complete scientific-computing API.

## Planned expansion

Future library work can add constants, additional trigonometric and transcendental functions, numeric utilities, string operations, file I/O, and time/system facilities. New functions should be added with explicit signatures, IR verification rules, backend behavior, documentation, and executable integration tests.
