# VEX

**Made for fun. Built to run.**

VEX is an experimental, general-purpose systems programming language. It aims to combine readable syntax with static typing, type inference, low-level capabilities, and performance-oriented native compilation.

VEX is an independent project for experimentation and practical learning. It is not intended to replace established programming languages.

## Status

The current C++20 development build has:

- immutable source buffers, byte-based source spans, and structured diagnostics;
- a lexer, parser, and initial abstract syntax tree;
- a semantic checker for lexical scopes, name resolution, primitive type inference, and static type validation;
- a command-line interface for token inspection, AST printing, and semantic checking;
- unit tests and a Linux, Windows, and macOS CI matrix.

VEX does **not** execute programs or generate C, object files, or machine code yet. Collection types, a runtime, and the memory model are not implemented.

## Design direction

- Static typing with inferred local binding types.
- Newlines separate statements; semicolons are not used.
- Curly braces delimit blocks; indentation is not semantic.
- Function calls use parentheses.
- A hybrid memory-management model is the selected direction, but ownership, borrowing, and allocation semantics remain open.
- Native compilation and a backend-independent compiler architecture are long-term goals.

These are design goals, not claims that every feature already exists.

## Build

Requirements: CMake 3.20+ and a C++20 compiler (GCC, Clang, or MSVC).

~~~sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build --config Release --output-on-failure
~~~

## Compiler commands

On Windows with a multi-configuration CMake generator:

~~~powershell
.\build\Release\vex.exe --help
.\build\Release\vex.exe --version
.\build\Release\vex.exe lex examples\hello.vex
.\build\Release\vex.exe parse examples\hello.vex
.\build\Release\vex.exe check examples\hello.vex
~~~

On Linux or macOS:

~~~sh
./build/vex --help
./build/vex --version
./build/vex lex examples/hello.vex
./build/vex parse examples/hello.vex
./build/vex check examples/hello.vex
~~~

- `lex` prints token kinds and source positions.
- `parse` prints the parsed abstract syntax tree.
- `check` resolves identifiers and validates the supported static type rules.

None of these commands executes the program.

## Example syntax direction

~~~vex
make main() {
    let message = "Hello, VEX!"
    say(message)
}
~~~

The checker recognizes `say(value)` as a built-in output intrinsic for type checking. No output runtime or executable backend exists yet.

## Project documents

- [Working language specification](SPEC.md)
- [Compiler architecture](docs/architecture.md)
- [Compiler design review and references](docs/compiler-design-review.md)

## License

VEX is distributed under the MIT License. See [LICENSE](LICENSE).
