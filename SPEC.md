# VEX language specification — working draft

**Version:** 0.1 design draft  
**Status:** Experimental; syntax and semantics may change.

VEX is an experimental, general-purpose systems programming language. It is being designed to combine readable syntax with static typing, explicit low-level capabilities, and native compilation. It is made for experimentation and practical learning; it is not intended to replace established languages.

This document separates decisions already made for the project from details that remain open. A design goal is not automatically an implemented feature.

## 1. Core design decisions

- **Typing:** statically typed, with type inference for local bindings where the compiler can determine a type.
- **Memory management direction:** hybrid. The intended design combines automatic lifetime management for ordinary values with explicit low-level memory capabilities. The ownership, borrowing, and allocation rules are not finalized yet.
- **Statement separation:** newlines separate statements. Semicolons are not part of VEX syntax.
- **Blocks:** curly braces delimit blocks. Indentation is for readability and does not determine program structure.
- **Calls:** function calls use parentheses, including built-in calls such as `say("Hello")`.
- **Compiler implementation:** the initial compiler is written in C++20. The language itself is independent of that implementation choice.
- **Initial backend direction:** C code generation is the planned first route to native executables; it is not implemented yet.

## 2. Working syntax

The following examples express the current direction. They are design examples, not a claim that every construct already compiles.

### Program entry point and bindings

```vex
make main() {
    let name = "VEX"
    let version = 1
    const gravity: float = 9.81

    say("Welcome to {name}!")
    say(version)
}
```

- `make` introduces a function.
- `let` introduces a mutable binding.
- `const` introduces an immutable binding.
- Type annotations use `name: type` syntax and are optional where inference is possible.
- `main` is the proposed program entry point.
- `say(...)` is intended as a standard-library output function, not a reserved keyword.

String interpolation shown as `{name}` is a proposed feature; its exact escaping rules remain to be specified.

### Functions

```vex
make calculate_force(mass: float, acceleration: float) -> float {
    return mass * acceleration
}
```

### Conditions

```vex
if temperature > 30 {
    say("It's hot!")
} else {
    say("It's not hot!")
}
```

### Repetition and iteration

```vex
repeat 5 times {
    say("VEX!")
}

for number in numbers {
    say(number)
}
```

The final iteration syntax and collection semantics are still under design.

## 3. Initial type direction

The initial built-in type set is planned to include:

| Type | Intended meaning |
|---|---|
| `int` | Signed 64-bit integer |
| `float` | 64-bit IEEE 754 floating-point value |
| `bool` | Boolean value: `true` or `false` |
| `string` | UTF-8 string value |

Literal forms and type inference will be specified precisely before semantic analysis is implemented. Character, byte, pointer, array, and structure types are future design areas, not part of the currently finalized core type set.

## 4. Newline and whitespace direction

- A newline is a statement separator outside an expression continuation or a parenthesized/bracketed continuation.
- Spaces and tabs are insignificant except inside string literals.
- Indentation has no semantic meaning.
- Line comments begin with `//`.
- Block comments use `/* ... */` and do not nest in the initial design.
- Semicolons are rejected; use newlines to separate statements.

The parser specification will define exact continuation behavior, including line breaks around operators and delimiters, before parsing is implemented.

## 5. Memory model direction

The project has selected a hybrid memory-management direction, but not yet a complete memory model. The design work still required includes:

- ownership and lifetime rules;
- borrowing and reference rules;
- deterministic destruction and cleanup;
- rules for stack versus heap storage;
- explicit allocation and deallocation APIs;
- pointer semantics and safety boundaries;
- interaction with collections and strings.

No default tracing garbage collector is currently planned. This is a design preference, not a finalized or implemented guarantee.

## 6. Compiler milestones

1. Lexer and token diagnostics.
2. Parser and AST.
3. Name resolution and static type checking.
4. Intermediate representation.
5. C backend and native compilation.
6. Standard library, runtime, and memory model implementation.
7. Editor integration and cross-platform releases.

## 7. Open questions

The following must be settled before the corresponding features are implemented:

- exact integer overflow and numeric conversion behavior;
- whether `const` means immutable binding only or also compile-time evaluation;
- newline continuation rules;
- string interpolation and escape rules;
- function overloading and default arguments;
- array and structure layout;
- ownership, borrowing, and pointer safety;
- module and import syntax;
- ABI and foreign-function interface;
- target architecture and platform support.
