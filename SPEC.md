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
- **Compiler implementation:** the initial compiler is written in C++20. This does not constrain the language implementation forever.
- **Backend direction:** C code generation is a possible first bootstrap route, not an implemented feature or a permanent architectural constraint.

## 2. Provisional syntax

The examples below describe the current design direction. The parser does not exist yet, so these examples are not executable.

### Functions and bindings

```vex
make main() {
    let name = "VEX"
    let version = 1
    const gravity: float = 9.81

    say("Welcome to VEX!")
    say(version)
}
```

- `make` introduces a function.
- `let` introduces a mutable binding.
- `const` introduces an immutable binding.
- Type annotations use `name: type` syntax and may be omitted when inference is possible.
- `main` is the proposed program entry point.
- `say(...)` is intended to be a standard-library function, not a reserved keyword.

### Functions with parameters and return types

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

The exact iteration protocol, collection model, and loop-control semantics remain open.

String interpolation such as `{name}` inside a string is not committed syntax; it needs a separate escaping and parsing design before implementation.

## 3. Initial built-in type direction

| Type | Intended meaning |
|---|---|
| `int` | Signed 64-bit integer |
| `float` | 64-bit IEEE-754 floating-point value |
| `bool` | Boolean value: `true` or `false` |
| `string` | UTF-8 string value |

This is a provisional built-in set. Character, byte, pointer, array, structure, and function types need further design.

## 4. Lexical rules implemented in the current frontend

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

The lexer records byte-based half-open source spans. Parser-level newline continuation rules are still to be specified.

## 5. Memory model direction

The selected direction is hybrid, but a complete memory model has not been designed. Work still required includes:

- ownership and lifetime rules;
- borrowing and reference rules;
- deterministic destruction and cleanup;
- stack versus heap storage;
- explicit allocation and deallocation APIs;
- pointer semantics and safety boundaries;
- interaction with collections and strings.

No default tracing garbage collector is currently planned. This is a preference, not a finalized guarantee.

## 6. Compiler milestones

1. Source management, diagnostics, and lexing.
2. Parser and AST.
3. Name resolution and static type checking.
4. Backend-independent intermediate representation.
5. Initial code-generation backend.
6. Runtime, standard library, and memory model implementation.
7. Native toolchain integration and editor tooling.
8. Optimization, releases, and eventual self-hosting research.

## 7. Open language-design questions

- exact integer overflow and numeric conversion behavior;
- whether `const` means immutable binding only or compile-time evaluation;
- expression continuation across newlines;
- string interpolation and escaping;
- function overloading and default arguments;
- array and structure layout;
- ownership, borrowing, and pointer safety;
- module and import syntax;
- ABI and foreign-function interface;
- target architectures and platform support.
