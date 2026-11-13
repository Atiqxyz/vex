# VEX language specification — working draft

**Version:** 0.1 design draft  
**Status:** Experimental; syntax and semantics may change.

VEX is an experimental, general-purpose systems programming language. It is being designed to combine readable syntax, static typing, low-level capabilities, and native compilation. It is an independent project made for experimentation and practical learning.

This document distinguishes current project decisions from provisional syntax and unresolved semantics. A design goal is not an implemented feature.

## 1. Decisions already made

- **Typing:** statically typed, with type inference for local bindings when the compiler can determine a type.
- **Memory-management direction:** hybrid. The exact ownership, borrowing, lifetime, allocation, and cleanup rules are not finalized.
- **Statement separation:** newlines separate statements. Semicolons are not part of VEX syntax.
- **Blocks:** curly braces delimit blocks. Indentation is for readability and has no structural meaning.
- **Calls:** function calls use parentheses, including standard-library calls such as `say("Hello")`.
- **Square brackets:** comma-separated values form fixed-size array literals; a single expression without a comma remains the existing multiline/grouping form. Postfix brackets index an array.
- **Compiler implementation:** the initial compiler is written in C++20. This does not constrain the language implementation forever.
- **Backend direction:** the IR is backend-independent; the first implemented source-generation target is portable C11. C is a bootstrap backend, not the language definition.

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
| 4 | ranges `..`, `..=` | non-associative |
| 5 | equality `==`, `!=` | left |
| 6 | comparison `<`, `<=`, `>`, `>=` | left |
| 7 | addition `+`, `-` | left |
| 8 | multiplication `*`, `/`, `%` | left |
| 9 | unary `+`, `-`, `!` | prefix |
| 10 | function call `(...)` | postfix |
| 11 | array indexing `[...]` | postfix |

The parser accepts assignment to a local name or a simple named-array index such as `values[index] = value`. Nested member/index assignment and compound assignment to an array element are not implemented.

## 4. Initial static semantics implemented

The current semantic checker implements primitive types, inferred fixed-array shapes, and a temporary integer-range type used only for direct `for-in` iteration. An array's element type and length are determined by its literal; array type annotations and array function parameters/returns are not implemented yet. Range expressions are not first-class values and cannot be assigned, stored, or passed to functions. A comma is required for an array literal with one element (for example, `[value,]`); `[value]` remains a grouped expression.

| Type | Meaning |
|---|---|
| `int` | Signed 64-bit integer direction |
| `float` | 64-bit IEEE-754 floating-point direction |
| `bool` | Boolean value |
| `string` | UTF-8 string value |
| `void` | Function return with no value |
| fixed array | Inferred, one-dimensional, fixed-length sequence of primitive values |
| integer range | Temporary half-open or inclusive iteration bounds; not a storable value |

The implemented numeric representation is signed 64-bit integers and 64-bit floating-point values. Fixed arrays are stored as value aggregates; they do not allocate heap memory.

- Local `let` and `const` types are inferred from their initializers when no annotation is present.
- Explicit local annotations are checked against initializer types.
- An `int` value may widen to `float`; implicit conversion from `float` to `int` is rejected.
- Arithmetic operators require numeric operands. The remainder operator `%` requires two `int` operands.
- Relational comparisons require numeric operands. Equality supports compatible primitive types; whole-array equality is rejected.
- Logical operators, `if` conditions, and `while` conditions require `bool`.
- `repeat` counts require `int`.
- `for-in` accepts fixed arrays and integer ranges. Range endpoints require `int`; `start..end` is half-open and `start..=end` is inclusive, both iterating upward by one. Endpoints are evaluated once before entering the loop.
- `break` and `continue` are only valid inside loops and target the innermost enclosing loop.
- Compound assignments are shorthand for their corresponding arithmetic operation followed by assignment; they use the same operand, mutability, and conversion checks as ordinary assignment.
- Variables are visible after declaration in their lexical scope. Nested blocks may shadow outer bindings; duplicate declarations in the same scope are errors.
- Assignment to `let` is allowed when the value is type-compatible. Assignment to `const` is rejected.
- Function parameters require explicit types. Function signatures are registered before body checking, enabling forward calls. Argument count and types are checked.
- A function without a return annotation is treated as `void`. A non-void function must return a compatible value on every control-flow path recognized by the current checker.
- The built-in `say(value)` accepts one supported primitive value and returns `void`; arrays must be indexed before printing.

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
- stack versus heap storage;
- explicit allocation and deallocation APIs;
- pointer semantics and safety boundaries;
- interaction with collections and strings.

No default tracing garbage collector is currently planned. This is a preference, not a finalized guarantee.

## 7. Compiler milestones

1. Source management, diagnostics, and lexing.
2. Parser and AST.
3. Initial name resolution and static type checking.
4. Typed, backend-independent intermediate representation (initial SSA IR, verifier, and lowering implemented).
5. Initial C source-generation backend (implemented); executable generation and native code generation remain future work.
6. Runtime, standard library, and memory model implementation.
7. Native toolchain integration and editor tooling.
8. Optimization, releases, and eventual self-hosting research.

### Initial typed IR implemented

The compiler exposes a typed, backend-independent SSA intermediate representation for the currently supported language subset.

- Values have explicit primitive IR types and unique SSA identifiers.
- Functions contain ordered basic blocks; each block has one explicit terminator.
- Basic-block parameters represent values passed along incoming control-flow edges. They provide SSA merge points without requiring phi instructions in the IR syntax.
- The instruction set covers constants, numeric and logical operations, comparisons, integer-to-float widening, direct calls, the `say` intrinsic, and fixed-array construction, indexing, and updates.
- The lowering pass consumes semantic-model information instead of repeating name resolution or type inference.
- `if` branches, short-circuit logical expressions, counted `repeat` loops, condition-driven `while` loops, fixed-array iteration, integer-range iteration, and loop-control edges lower into explicit control-flow graphs.
- The verifier checks instruction and call types, array element types and shapes, SSA definitions and uses, branch arguments, terminators, reachability, and dominance.
- Source spans are retained on IR values and instructions.

### Initial C backend implemented

The `vex emit-c <file.vex>` command runs lexing, parsing, semantic analysis, IR lowering, and IR verification, then emits portable C11 source to standard output.

- Every current IR instruction and terminator has a C representation.
- VEX `int`, `float`, `bool`, `string`, and `void` map to `int64_t`, `double`, `bool`, a length-aware byte string, and `void`.
- Basic-block parameters are emitted as local SSA variables with parallel edge-copy temporaries, preserving merge and loop semantics.
- Direct calls use generated private C symbols, with prototypes emitted before definitions so forward VEX calls work.
- `say` prints integers, floating-point values, booleans, and strings followed by a newline.
- A zero-argument VEX `main()` is adapted to the C entry point. It may return `void` or `int`; an integer result must be in the process exit-code range 0 through 255.
- Signed integer arithmetic is checked. Overflow, division or remainder by zero, and the `INT64_MIN / -1` and `INT64_MIN % -1` cases produce a runtime failure instead of invoking C signed-overflow undefined behavior.
- Fixed arrays use generated aggregate layouts. Index reads and writes check negative and out-of-range indices at runtime.
- String literals are decoded from VEX escapes and emitted as length-aware byte sequences, so embedded NUL bytes are retained.

The backend emits C source only. It does not invoke a C compiler, link an executable, or execute VEX programs. Those are the next integration milestone.

## 8. Open language-design questions

- whether `const` means immutable binding only or compile-time evaluation;
- global variables shared between separately declared functions (not implemented; current top-level bindings belong to implicit-entry execution);
- string interpolation and escaping;
- function overloading and default arguments;
- array and structure layout;
- ownership, borrowing, and pointer safety;
- module and import syntax;
- ABI and foreign-function interface;
- target architectures and optimization strategy.
