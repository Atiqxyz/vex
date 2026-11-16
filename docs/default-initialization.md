# Default initialization

**Status:** Implemented for explicitly typed mutable scalar bindings. Aggregate defaults remain deferred.

VEX supports a declaration without an initializer when the binding is mutable and has an explicit scalar type:

```vex
make main() {
    let iterations: int
    let ratio: float
    let active: bool
    let marker: byte
    let label: string
}
```

The language defines these initial values:

| Type | Default |
|---|---|
| `int` | `0` |
| `byte` | `0` |
| `float` | `0.0` |
| `bool` | `false` |
| `string` | Empty string |

These are deterministic values represented in typed IR and emitted consistently by the C and LLVM backends. Numeric and boolean defaults do not require heap allocation; the empty string uses the backend's normal static string representation.

## Current restrictions

- `const` declarations require an explicit initializer.
- A declaration without an initializer requires an explicit type annotation; inference has no value from which to infer a type.
- Arrays and structures require explicit initializers. Their default semantics must account for fixed aggregate size, dynamic-array headers, nested values, and resource lifetime before they are enabled.
- This feature does not create uninitialized storage and does not introduce garbage collection or implicit heap allocation.

## Design direction

Defaults are convenience, not a loss of control. The compiler still knows the declared type, emits a concrete initial value, and preserves predictable native representation. A future explicit uninitialized-storage facility, if adopted, will be a separate opt-in feature with defined safety and lifetime rules. It will not be the implicit behavior of an ordinary declaration.
