# VEX language specification — working draft

**Version:** 0.1 design draft  
**Status:** Experimental; syntax and semantics may change.

VEX is an experimental, general-purpose systems programming language. It is being designed to combine readable syntax, static typing, low-level capabilities, and native compilation. It is an independent project made for experimentation and practical learning.

This document distinguishes current project decisions from provisional syntax and unresolved semantics. A design goal is not an implemented feature.

## 1. Decisions already made

- **Typing:** statically typed, with type inference for local bindings when the compiler can determine a type.
- **Memory-management direction:** hybrid. Dynamic arrays use shared heap references tracked for the lifetime of a generated program; concatenated string buffers use the same program-lifetime cleanup boundary. Nested fixed aggregate values are inline and subject to target-native 256 KiB per-object and 512 KiB per-function nested-fixed storage limits. The wider ownership, borrowing, general stack/heap, and per-scope lifetime model remains unfinished.
- **Statement separation:** newlines separate statements. Semicolons are not part of VEX syntax.
- **Blocks:** curly braces delimit blocks. Indentation is for readability and has no structural meaning.
- **Calls:** function calls use parentheses, including standard-library calls such as `say("Hello")`.
- **Square brackets:** comma-separated values form fixed-size array literals; a single expression without a comma remains the existing multiline/grouping form. Postfix brackets index an array.
- **Compiler implementation:** the initial compiler is written in C++20. This does not constrain the language implementation forever.
- **Self-hosting capability:** VEX is not required to be self-hosted, but its language, runtime, and build tooling should eventually be capable of implementing the full compiler in VEX.
- **Backend direction:** the IR is backend-independent and has C11 and LLVM code-generation backends. LLVM is the default native build path; C remains an optional bootstrap/fallback backend. Neither backend defines language semantics.

## 2. Provisional syntax

The examples below describe the current design direction. The parser and semantic checker implement the supported subset, and valid programs lower through typed SSA IR to the C or LLVM backend. A design goal is not an implemented feature.

### Functions and bindings

~~~vex
make main() {
    let message = "Hello, VEX!"
    let version = 1
    const gravity: float = 9.81

    say("Welcome to VEX!")
    say(version)
}
~~~

- `make` introduces a function.
- `let` introduces a mutable binding.
- `const` introduces an immutable binding.
- Type annotations use `name: type` syntax and may be omitted on local bindings when inference is possible.
- A mutable, explicitly typed scalar binding may omit its initializer: `int` and `byte` default to zero, `float` to 0.0, `bool` to false, and `string` to the empty string. Array and structure declarations still require explicit initializers.
- `const` declarations and declarations relying on type inference require explicit initializers. These defaults are defined language values, not uninitialized storage.
- Function parameters require explicit type annotations in the current semantic checker.
- A function without a return type annotation is treated as returning `void`; value-returning functions must declare a return type.
- If `make main()` is present, it is the program entry point and top-level executable statements are rejected. If it is absent, top-level statements execute in source order through a compiler-generated entry function. File-scope function declarations remain declarations in either mode. File-scope values belong to implicit-entry execution and are not captured by ordinary functions; pass values as parameters instead.
- The built-in `say(value)` accepts one supported primitive value and is implemented by the initial C backend runtime.

### Functions with parameters and return types

~~~vex
make calculate_force(mass: float, acceleration: float) -> float {
    return mass * acceleration
}
~~~

Top-level function signatures are collected before function bodies are checked, so functions may call functions declared later in the file. Function overloading is not implemented.


### User-defined structures

VEX supports named, statically typed value structures:

~~~vex
struct Point {
    x: float,
    y: float,
}

make translate(point: Point, dx: float) -> Point {
    let result = point
    result.x = result.x + dx
    return result
}

make main() {
    let point = Point { x: 1.0, y: 2.0 }
    let moved = translate(point, 4.0)
    say(moved.x)
}
~~~

- A `struct` declaration is file-scope only. Field names must be unique, and a structure must have at least one field.
- A field type may be a primitive, an earlier-declared structure, or a fixed/dynamic nested array of either. Repeated dimensions support dynamic, mixed, and all-fixed jagged shapes (for example, `Point[][]`, `int[2][]`, and `int[2][3]`). Nested fixed inline storage is subject to the target-aware object and per-function budgets described below.
- A fixed array stores its elements by value. Fixed arrays of structures can be fields, local values, function parameters, and return values.
- A dynamic array stores a shared runtime-managed reference to contiguous elements. Dynamic arrays of structures are supported; copying a structure that contains a dynamic array copies that reference, so the nested dynamic collection remains shared.
- Direct recursive by-value fields and fixed arrays that would create a by-value cycle are rejected. A dynamic array field may refer to its own containing structure because it is pointer-backed (for example, `Node { children: Node[] }`). Forward structure references remain unsupported.
- Every structure literal must initialize every declared field exactly once using named fields. Field initializer expressions are evaluated in written order; the resulting value is laid out in declaration order.
- Use `TypeName { field: value, ... }` to construct a value and `value.field` to read a field. Arrays of structures support normal indexing and field access, including paths such as `scene.points[index].x`.
- `let` structure bindings permit field updates. A `const` structure binding is immutable, including all of its fields.
- Structures have value semantics. Assignment, parameter passing, and return copy the structure value. Nested structure fields and fixed-array fields are copied as part of the containing value. Dynamic-array fields copy their shared reference, so the referenced collection remains aliased across structure copies.
- Struct values are not currently comparable with `==` or `!=`; define equality explicitly in a future language design before enabling it.
- Structure fields retain declaration order. Each field is placed according to the target's native ABI alignment for its type; the implementation may insert padding between fields and at the end of a structure. The structure's allocation size therefore includes any required tail padding and is not necessarily the sum of its field sizes.
- The C backend relies on the target C compiler's native structure layout and `sizeof` when storing dynamic-array elements. The LLVM backend uses native aggregate types and derives allocation sizes from the target data layout. Both paths must therefore use the target ABI's full structure allocation size, including tail padding, for dynamic-array element strides.
- Structure sizes, alignments, and field offsets may differ between target ABIs. VEX does not promise a stable cross-platform byte representation, and structure memory must not be treated as a serialization format. Explicit packed layout, user-selected alignment, ABI annotations, and raw pointers beyond the limited primitive-local subset described below are not implemented.

### User-defined enums and exhaustive matching

VEX supports nominal inline tagged unions. Each enum declaration defines a distinct type with a closed set of variants:

~~~vex
enum SolverResult {
    Converged(int, float),
    Diverged(string),
    Cancelled
}

make describe(result: SolverResult) {
    match result {
        SolverResult.Converged(iterations, residual) => {
            say(iterations)
            say(residual)
        }
        SolverResult.Diverged(reason) => {
            say(reason)
        }
        SolverResult.Cancelled => {
            say("cancelled")
        }
    }
}
~~~

- A variant may contain zero or more positional payload fields. Variant names are unique within the enum; payload positions and types are fixed by the declaration.
- Construct a variant using its qualified name, for example `SolverResult.Converged(42, 0.000001)` or `SolverResult.Cancelled`.
- Enum values have nominal identity. There is no implicit conversion between an enum and an integer.
- Enum payloads are stored inline. Fixed arrays and structures are copied by value; dynamic-array and string payloads retain their existing reference-backed descriptor behavior. Construction does not box a variant or allocate implicitly.
- The discriminant is an unsigned 32-bit value. Tags are assigned from zero in declaration order. The payload area is large enough and aligned for the largest variant; the shared target-layout service calculates the complete representation.
- `match` is currently a statement, not an expression. Every declared variant must appear exactly once, or a final wildcard arm must be present. Arms contain blocks and do not fall through.
- The match scrutinee is evaluated once. Each arm has its own lexical scope. Positional payload bindings are immutable and visible only within that arm.
- Named payload fields, guards, nested destructuring, user-selected tag widths, explicit tag values, generic enums, and expression-valued matching are not implemented.

This is VEX's current enum subset; see the [enum and tagged-union design](docs/enum-and-tagged-union-design.md) for the representation and verifier/backend contract.

### Conditions

~~~vex
if temperature > 30 {
    say("It's hot!")
} else {
    say("It's not hot!")
}
~~~

The condition of an `if` statement must have type `bool`.

### Repetition and iteration

~~~vex
repeat 5 times {
    say("VEX!")
}

while temperature > 30 {
    temperature -= 1
    if temperature == 25 {
        break
    }
}

for number in numbers {
    say(number)
}

for index in 0..10 {
    say(index)
}

for index in 1..=10 {
    say(index)
}
~~~

A `repeat` count must have type `int`. A `while` condition must have type `bool`. `break` exits the innermost enclosing loop and `continue` advances to its next iteration; both are rejected outside loops. `for-in` iterates fixed-size arrays or integer ranges and infers the loop variable type. The half-open `start..end` form includes the start and excludes the end; `start..=end` includes both endpoints. Range endpoints must be `int`, are evaluated once before iteration, and advance upward by one. A range with a start greater than its end is empty. Ranges are currently supported directly in `for-in`, not as storable values.

### Evaluation order and nested loops

VEX uses deterministic left-to-right evaluation for the expression forms currently supported:

- Ordinary binary expressions evaluate the left operand completely before the right operand.
- Function-call arguments and array-literal elements evaluate in their written order, from first to last.
- An indexed read evaluates its array expression before its index expression. An indexed assignment evaluates the index expression before the right-hand side.
- `&&` evaluates its right operand only when the left operand is `true`. `||` evaluates its right operand only when the left operand is `false`.
- A `repeat` count and a `for-in` iterable are evaluated once before the loop begins. A `while` condition is evaluated before each iteration. For an integer range, the start expression is evaluated once before the end expression; both are evaluated once before iteration.

When loops are nested, `break` exits only the innermost enclosing `while`, `repeat`, or `for-in` loop. `continue` advances only the innermost enclosing loop. Leaving an inner loop resumes execution in its enclosing loop.


### Multiline expressions

~~~vex
let force = [
    mass *
    acceleration
]
~~~

A newline ends an expression unless it is inside the explicit square-bracket grouping form. Commas distinguish array literals from a single grouped expression. Parentheses remain ordinary grouping and function-call syntax; they do not silently enable multiline continuation in this initial grammar.

## 3. Initial parser grammar

The parser currently accepts this subset:

~~~text
program          := newline* (function | statement)* EOF
function         := "make" identifier "(" parameters? ")" ("->" type)? block
parameters       := parameter ("," parameter)*
parameter        := identifier (":" type)?
statement        := variable | return | if | while | repeat | for
                  | break | continue | block | expression
variable         := ("let" | "const") identifier (":" type)? "=" expression
return           := "return" expression?
if               := "if" expression block ("else" (if | block))?
while            := "while" expression block
repeat           := "repeat" expression "times" block
for              := "for" identifier "in" expression block
range_expression := expression (".." | "..=") expression
break            := "break"
continue         := "continue"
block            := "{" newline* statement* "}"
multiline         := "[" newline* expression newline* "]"
array_literal     := "[" expression "," expression ("," expression)* ","? "]"
index             := postfix "[" expression "]"
postfix           := primary (call_suffix | index_suffix)*
call_suffix       := "(" arguments? ")"
index_suffix      := "[" expression "]"
~~~

Expression operators are parsed with these precedence levels, from lowest to highest:

| Level | Operators | Associativity |
|---:|---|---|
| 1 | assignment `=`, `+=`, `-=`, `*=`, `/=`, `%=` | right |
| 2 | logical OR `||` | left |
| 3 | logical AND `&&` | left |
| 4 | bitwise OR `|` | left |
| 5 | bitwise XOR `^` | left |
| 6 | bitwise AND `&` | left |
| 7 | ranges `..`, `..=` | non-associative |
| 8 | equality `==`, `!=` | left |
| 9 | comparison `<`, `<=`, `>`, `>=` | left |
| 10 | shifts `<<`, `>>` | left |
| 11 | addition `+`, `-` | left |
| 12 | multiplication `*`, `/`, `%` | left |
| 13 | unary `+`, `-`, `!`, `~`, address-of `&`, dereference `*` | prefix |
| 14 | function call `(...)` | postfix |
| 15 | array indexing `[...]` | postfix |
| 16 | structure member access and construction | postfix |

The parser and semantic analyzer accept assignment to a named binding and nested aggregate paths rooted at a named structure or array, including `values[index] = value`, `points[index].x = value`, and compound forms such as `scene.points[index].x += amount`. The compiler evaluates the target path from the named root outward and evaluates each index expression exactly once. For compound assignment, it reads the old target value after resolving the path, then evaluates the right-hand side, applies the operator, and rebuilds the updated aggregate back to the root binding. For plain assignment, the replacement expression is evaluated after the full target path has been resolved.

## 4. Initial static semantics implemented

The semantic checker implements primitive types, fixed and dynamic arrays with recursive nested jagged shapes, structures, and a temporary integer-range type used only for direct `for-in` iteration. Array annotations use a positive length for fixed dimensions and omit the length for dynamic dimensions (for example, `Point[3]`, `int[][]`, `int[2][]`, `int[][4]`, and `int[2][3]`). Each dynamic dimension has its own header, length, and capacity; nested rows may be ragged. Fixed dimensions are inline value aggregates, while dynamic dimensions are shared heap-backed references, including when nested inside fixed arrays or structures. Indexing removes one dimension at a time; nested reads, writes, compound assignment, iteration, collection operations, and function parameters/returns preserve the remaining shape. Nested fixed inline storage is limited to 256 KiB per materialized object and 512 KiB per function across nested-fixed aggregate slots and backend temporaries. The semantic checker rejects nested dimension runs whose element-count lower bound alone exceeds the object limit; C checks exact target-native `sizeof` values at compile time, while LLVM checks target-layout allocation sizes before allocating stack slots. This is a bounded nested-aggregate policy, not a complete stack model. The [nested-array design note](docs/nested-array-design.md) records the full semantics and remaining limitations. Fixed arrays are value aggregates and are passed to/returned from functions by value with exact leaf-type, structure-identity, and dimension checks. Dynamic arrays are heap-backed shared references: assignment, argument passing, and return copy the reference, not the elements. This applies recursively to nested arrays and dynamic-array fields inside structures. Dynamic-array element reads and writes are type-checked and bounds-checked. Empty literals require an explicit dynamic-array annotation at each ambiguous empty dimension, such as `let values: Point[] = []` or `let rows: int[][] = [[], [1, 2]]`.

Dynamic-array headers and their backing buffers are registered in a program-lifetime runtime list. Every registered array allocation is released when the generated entry function returns, including when its integer result is outside the process exit-code range (the runtime cleans up before trapping). Dynamically allocated string concatenation and joining results are tracked separately and released at the same point; string values preserve embedded NUL bytes. The C and LLVM backends check allocation results and allocation-size arithmetic before updating runtime headers. A runtime trap terminates the process without language-level unwinding; the operating system reclaims memory. This policy does not release arrays when they leave lexical scope, are popped or overwritten, or become unreachable through aliases, so a long-running program can retain such allocations until entry return. There is currently no per-scope release, reference counting, borrowing checker, or manual deallocation API. See the [runtime allocation audit](docs/memory-runtime-audit.md) for the cross-backend review and its testing boundary. The join(string[]) built-in accepts a dynamic array of strings, checks the combined byte length for overflow, allocates the output once, and copies each non-empty chunk once. Empty output is returned without allocation. A chunk-based builder can be composed from a structure containing string[], push, and join; array growth is geometric and join produces an immutable snapshot. Range expressions are not first-class values and cannot be assigned, stored, or passed to functions. A comma is required for an array literal with one element (for example, `[value,]`); `[value]` remains a grouped expression.

| Type | Meaning |
|---|---|
| `int` | Signed 64-bit integer |
| `byte` | Unsigned 8-bit integer, with values from 0 through 255 |
| `float` | 64-bit IEEE-754 floating-point direction |
| `bool` | Boolean value |
| `string` | Length-aware byte string; UTF-8 validity is not guaranteed. |
| `void` | Function return with no value |
| `*T` | Single-level typed pointer to a mutable local or parameter whose pointee is a supported primitive, structure, or enum type; pointer values may be passed as function parameters |
| fixed array | One inline fixed-length dimension in an array shape; may contain supported nested dynamic dimensions, and may be annotated as `Point[3]` or `int[2][]` |
| integer range | Temporary half-open or inclusive iteration bounds; not a storable value |

The implemented numeric representation is signed 64-bit `int` values and 64-bit floating-point values. `byte` is a separate unsigned 8-bit scalar and is deliberately not an arithmetic numeric type. `to_byte(int)` checks that the value is in the inclusive range 0 through 255 and traps otherwise; `to_int(byte)` zero-extends the value to `int`. Byte relational comparisons are unsigned. Bitwise operations accept two `int` operands or two `byte` operands: `int` operations act on all 64 bits, while `byte` operations act on exactly 8 bits and return `byte`. Fixed arrays are stored as value aggregates; they do not allocate heap memory. Dynamic arrays store a pointer to a runtime-managed header and a growable contiguous element buffer. Dynamic-array elements are copied as typed values, including whole structures; any dynamic-array fields inside those structures retain their shared references.

- Local `let` and `const` types are inferred from their initializers when no annotation is present.
- A mutable local or mutable function parameter of a supported value type may be addressed with unary `&`; the initial native pointer subset supports primitive, structure, and enum pointees. `*T` is the explicit single-level pointer annotation, and a local pointer may also be inferred from an address-of initializer. Unary `*` dereferences a pointer for a read, while `*pointer = value` and compound assignments such as `*pointer += 1` write through it. Fields and array elements may be projected from a dereferenced structure, for example `(*pointer).field = value`. Pointer pointee types, including nominal structure/enum identity, must match exactly. A pointer-returning function may return a pointer parameter directly, preserving that borrowed parameter's pointee type and origin; returning a pointer to the function's own local storage is rejected. This initial return rule does not yet constitute a complete lifetime or aliasing system. Pointer-to-pointer values, null pointers, pointer arithmetic, integer/pointer conversions, and an `unsafe` construct are not implemented. A local pointer may not be rebound to a target from a shorter-lived lexical scope. This remains a limited lexical escape check, not a complete lifetime, provenance, or borrowing model.
- Explicit local annotations are checked against initializer types.
- An `int` value may widen to `float`; implicit conversion from `float` to `int` and from `int` to `byte` is rejected. Use checked `to_byte(int)` and explicit `to_int(byte)` conversions.
- Addition `+` accepts either two numeric operands or two strings. String concatenation preserves byte lengths, including embedded NUL bytes; it does not decode or normalize UTF-8. Subtraction, multiplication, division, and remainder require numeric operands. The remainder operator `%` requires two `int` operands and uses truncating signed-remainder semantics: the quotient is truncated toward zero and any non-zero remainder has the sign of the left operand (for example, `-7 % 3 == -1`, `7 % -3 == 1`, and `-7 % -3 == -1`). A zero divisor traps. The defined edge case `INT64_MIN % -1` returns `0`; `INT64_MIN / -1` remains a checked division overflow.
- Relational comparisons require numeric operands or two `byte` operands; byte comparisons are unsigned. Equality supports compatible primitive types; whole-array equality is rejected.
- Logical operators, `if` conditions, and `while` conditions require `bool`.
- Bitwise `&`, `|`, and `^` operate on matching `int` or `byte` operands and preserve the operand type. Unary `~` inverts all 64 bits for `int` or all 8 bits for `byte`. They are distinct from short-circuit logical `&&` and `||`.
- `<<` and `>>` accept an `int` or `byte` left operand and an `int` count. Counts outside `0..63` for `int` or `0..7` for `byte` trap. Left shift operates on the fixed-width bit pattern, shifts in zero bits, and discards high bits. `int >>` is arithmetic (sign-extending); `byte >>` is logical (zero-filling). The result retains the left operand's type. Shifts bind less tightly than addition and more tightly than comparisons.
- `rotate_left(value, count)` and `rotate_right(value, count)` accept an `int` or `byte` value and an `int` count, preserving the value type. They rotate all 64 or 8 bits, respectively. The count is normalized modulo the operand width, so negative and oversized counts are defined; a count of one full width leaves the value unchanged.
- `repeat` counts require `int`.
- `for-in` accepts fixed arrays and dynamic arrays of primitives or structures, plus integer ranges. Fixed-array lengths are known at compile time; dynamic-array lengths are re-read at each loop header, so appending or removing elements during iteration can change the number of iterations. Range endpoints require `int`; `start..end` is half-open and `start..=end` is inclusive, both iterating upward by one. Endpoints are evaluated once before entering the loop.
- `break` and `continue` are only valid inside loops and target the innermost enclosing loop.
- Compound assignments `+=`, `-=`, `*=`, `/=`, and `%=` apply their corresponding operation and store the result. They work on named bindings and nested field/index paths rooted at named aggregate bindings. The `+=` operator also concatenates two strings; `%=` requires two `int` operands. The result must be storable in the target type, and immutable roots cannot be modified.
- Variables are visible after declaration in their lexical scope. Nested blocks may shadow outer bindings; duplicate declarations in the same scope are errors.
- Assignment to `let` is allowed when the value is type-compatible. Assignment to `const` is rejected.
- Function parameters require explicit types. Primitive, structure, and array annotations such as `point: Point`, `values: int[3]`, `raw: byte[]`, and `points: Point[]` are supported. Function signatures are registered before body checking, enabling forward calls. Argument count, element types, and fixed-array lengths are checked.
- Fixed arrays of primitives or structures may be passed to and returned from functions by value. A function returning `Point[3]` must return exactly three values of that nominal structure type; an array argument must match the parameter's element type, structure identity, and full fixed shape. Dynamic arrays may also be passed to and returned from functions, but they are shared references rather than copied collections; their element type and nominal structure identity must match the declared `T[]` shape.
- A function without a return annotation is treated as `void`. A non-void function is rejected when a reachable path can reach the end of its body without returning a compatible value. Return-path analysis follows sequential statements, nested blocks, conditional branches, and loop exits. It recognizes literal boolean conditions, positive or non-positive integer literal repeat counts, and fixed-array or integer-range iteration when non-emptiness can be proven from the source. A loop that may execute zero times cannot by itself satisfy a return requirement. A path proven unable to reach the function end (for example, a literal `while true` loop without a reachable `break`) does not need a return value.
- The built-in `say(value)` accepts one supported primitive value and returns `void`; arrays must be indexed before printing.
- `length(string)` returns the exact byte count stored in the string descriptor, including embedded NUL bytes; it does not count Unicode scalar values. `length(array)` returns the element count for fixed or dynamic arrays (fixed-array length is lowered as a constant). join(chunks) accepts a dynamic string[] and returns an immutable byte string containing each chunk in order, preserving embedded NUL bytes and using one allocation for non-empty output after checking cumulative length and allocation-size overflow. `push(array, value)` appends a matching primitive or structure value to a dynamic array and returns `void`. `pop(array)` removes and returns the last element, including a structure value; popping an empty dynamic array traps. `push` and `pop` reject fixed arrays and mutation through immutable aggregate bindings.


### Binary buffers, byte codecs, and file I/O

A dynamic `byte[]` is the current mutable binary-buffer type. It is separate from `string`; strings are length-aware byte sequences intended for text-like values and do not gain mutable buffer semantics.

- `byte_slice(source, start, count) -> byte[]` copies the specified byte range to an independent dynamic array. `start` and `count` must be non-negative and the whole range must be inside the source; otherwise the operation traps. This is an owning copy, not a borrowed view.
- `read_u16_le`, `read_u16_be`, `read_u32_le`, and `read_u32_be` accept `(byte[], int)` and return the decoded unsigned value as `int`.
- `write_u16_le`, `write_u16_be`, `write_u32_le`, and `write_u32_be` accept `(byte[], int, int)` and return `void`. The value must be in the range 0 through 65,535 for 16-bit writes or 0 through 4,294,967,295 for 32-bit writes. The target byte range must already exist; writes do not grow the buffer.
- Binary integer reads and writes trap for negative offsets or ranges outside the current buffer. Writes also trap for values outside the selected unsigned width.
- `read_file(path, destination) -> int` opens `path` in binary read mode and appends the file's bytes to the supplied mutable `byte[]`. It returns the appended byte count or `-1` for invalid paths or ordinary open/read/close failures. A failed read/close restores the destination's original logical length and bytes, although capacity may have grown.
- `write_file(path, bytes) -> int` opens in binary write mode (truncating an existing file), writes the complete buffer, and returns the byte count or `-1` on ordinary I/O failure. A failure after partial output may leave a partially written file.
- File paths must be non-empty and must not contain embedded NUL bytes. Both file operations close the file before returning; no long-lived handle is exposed.
- File-I/O failure reporting currently uses the `-1` sentinel. A general `Result` type, structured OS error values, streaming file handles, and directory APIs are not implemented.
- Buffer allocations follow the existing dynamic-array program-lifetime runtime model. Allocation failure traps. The language still has no borrowed slice or general ownership/borrowing checker.

The C and LLVM backends implement these operations with the same checked bounds, unsigned width, byte order, append/truncate, and error-return behavior. They are compiler-known runtime operations rather than importable modules.

## 5. Lexical rules implemented in the current frontend

- Identifiers currently use ASCII letters or `_` as the first character, followed by ASCII letters, digits, or `_`.
- Integer literals use decimal digits.
- Floating-point literals support a decimal fraction and/or an exponent, such as `3.5`, `1e6`, and `2.5e-3`.
- String literals use double quotes. Supported escapes are `\n`, `\r`, `\t`, `\0`, `\\`, `\"`, and `\'`.
- Raw line breaks are not allowed inside string literals.
- Line comments begin with `//`.
- Block comments use `/* ... */` and do not nest in the initial design.
- Newlines inside block comments are retained as statement separators.
- LF, CRLF, and CR line endings are recognized.
- Semicolons are rejected; use newlines to separate statements.
- Spaces and tabs are insignificant outside strings.

The lexer records byte-based half-open source spans. The parser treats a newline as a statement boundary except while parsing the explicit square-bracket grouping wrapper or an array literal.

## 6. Memory model direction

The selected direction is hybrid, but a complete memory model has not been designed. Work still required includes:

- ownership and lifetime rules;
- borrowing and reference rules;
- deterministic destruction and cleanup;
- general stack versus heap storage beyond the nested fixed aggregate budgets;
- explicit allocation and deallocation APIs;
- pointer semantics and safety boundaries;
- interaction with collections and strings.

No default tracing garbage collector is currently planned. This is a preference, not a finalized guarantee.

## 7. Compiler milestones

1. Source management, diagnostics, and lexing.
2. Parser and AST.
3. Initial name resolution and static type checking.
4. Typed, backend-independent intermediate representation (initial SSA IR, verifier, and lowering implemented).
5. C11 and LLVM code-generation backends, including native build/run integration (implemented for the current language subset).
6. Runtime, standard library, and memory model implementation (ongoing; now includes dynamic byte buffers, binary codecs, and whole-file binary I/O).
7. Native toolchain integration and editor tooling (initial integration implemented; ongoing).
8. Optimization, releases, and eventual self-hosting research.

### Initial typed IR implemented

The compiler exposes a typed, backend-independent SSA intermediate representation for the currently supported language subset.

- Values have explicit primitive IR types and unique SSA identifiers.
- Functions contain ordered basic blocks; each block has one explicit terminator.
- Basic-block parameters represent values passed along incoming control-flow edges. They provide SSA merge points without requiring phi instructions in the IR syntax.
- The instruction set covers constants, numeric and logical operations, comparisons, integer-to-float widening, checked integer-to-byte conversion, byte-width bitwise operations, byte-preserving string concatenation/joining and byte-length inspection, owning byte slices, endian-aware binary integer reads/writes, whole-file byte-array reads/writes, dynamic-array length/capacity/reservation and mutation, direct calls, the `say` intrinsic, typed fixed/dynamic and recursively nested jagged array construction and indexing, and structure construction/field access/updates including nested array-valued fields.
- The lowering pass consumes semantic-model information instead of repeating name resolution or type inference.
- `if` branches, short-circuit logical expressions, counted `repeat` loops, condition-driven `while` loops, fixed-array iteration, integer-range iteration, and loop-control edges lower into explicit control-flow graphs.
- The verifier checks instruction and call types, array element types, lengths and nominal structure identities, structure field aggregate shapes, SSA definitions and uses, branch arguments, terminators, reachability, and dominance.
- Source spans are retained on IR values and instructions.

### Initial C backend implemented

The `vex emit-c <file.vex>` command runs lexing, parsing, semantic analysis, IR lowering, and IR verification, then emits portable C11 source to standard output.

- Every current IR instruction and terminator has a C representation.
- VEX `int`, `byte`, `float`, `bool`, `string`, and `void` map to `int64_t`, `uint8_t`, `double`, `bool`, a length-aware byte string, and `void`. Dynamic `byte[]` buffers use the shared dynamic-array runtime.
- Basic-block parameters are emitted as local SSA variables with parallel edge-copy temporaries, preserving merge and loop semantics.
- Direct calls use generated private C symbols, with prototypes emitted before definitions so forward VEX calls work.
- `say` prints integers, floating-point values, booleans, and strings followed by a newline.
- A zero-argument VEX `main()` is adapted to the C entry point. It may return `void` or `int`; an integer result must be in the process exit-code range 0 through 255.
- Signed integer arithmetic is checked. Overflow, division or remainder by zero, and the `INT64_MIN / -1` and `INT64_MIN % -1` cases produce a runtime failure instead of invoking C signed-overflow undefined behavior.
- Fixed arrays use generated aggregate layouts. Index reads and writes check negative and out-of-range indices at runtime.
- String literals are decoded from VEX escapes and emitted as length-aware byte sequences, so embedded NUL bytes are retained.
- Byte-buffer operations use checked bounds and unsigned-width rules. `byte_slice` makes an independent copy; endian helpers encode/decode unsigned 16-/32-bit values.
- Whole-file `read_file`/`write_file` helpers use binary mode, chunked reads with geometric buffer growth, explicit `-1` I/O status, and close each file before returning.

The C backend can emit C11 source; the compiler's build/run commands can invoke the C backend toolchain. The default build/run commands use the LLVM backend and the bundled Zig/Clang toolchain. The language subset supported by both backends is covered by the cross-backend integration tests.

## 8. Open language-design questions

- whether `const` means immutable binding only or compile-time evaluation;
- global variables shared between separately declared functions (not implemented; current top-level bindings belong to implicit-entry execution);
- string interpolation and escaping;
- function overloading and default arguments;
- array and structure layout;
- ownership, borrowing, and pointer safety;
- module resolution, imported declarations, exports, and separate compilation (top-level quoted import syntax currently parses but is rejected during semantic analysis);
- ABI and foreign-function interface;
- target architectures and optimization strategy.
