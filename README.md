# VEX

**Made for fun. Built to run.**

VEX is an experimental, general-purpose systems programming language. It aims to combine readable syntax with static typing, type inference, low-level capabilities, and performance-oriented native compilation.

VEX is an independent project for experimentation and practical learning. It is not intended to replace established programming languages.

## Status

VEX is in its early compiler-foundation stage. The current C++20 development build has:

- immutable source buffers with byte-based source spans;
- a lexer with stable token kinds and structured diagnostics;
- a token-inspection CLI;
- cross-platform CMake builds and frontend tests.

A parser, AST, semantic analysis, intermediate representation, code generation, and executable runtime are not implemented yet. Language syntax and semantics may change.

## Design direction

- Static typing with local type inference.
- Newlines separate statements; semicolons are not used.
- Curly braces delimit blocks; indentation is not semantic.
- Function calls use parentheses.
- A hybrid memory-management model is the selected direction, but ownership, borrowing, and allocation semantics remain open.
- Native compilation and a backend-independent compiler architecture are long-term goals.

These are design goals, not claims that every feature already exists.

## Build

Requirements: CMake 3.20+ and a C++20 compiler (GCC, Clang, or MSVC).

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build --config Release --output-on-failure
```

## Inspect tokens

On Windows with a multi-configuration CMake generator:

```powershell
.\build\Release\vex.exe --help
.\build\Release\vex.exe --version
.\build\Release\vex.exe lex examples\hello.vex
```

On Linux or macOS:

```sh
./build/vex --help
./build/vex --version
./build/vex lex examples/hello.vex
```

The `lex` command prints token kinds and source positions. It does not parse or execute VEX programs.

## Example syntax direction

```vex
make main() {
    let message = "Hello, VEX!"
    say(message)
}
```

This illustrates the current design direction only. It is not executable in the current compiler build.

## Project documents

- [Working language specification](SPEC.md)
- [Compiler architecture](docs/architecture.md)
- [Compiler design review and references](docs/compiler-design-review.md)

## License

VEX is distributed under the MIT License. See [LICENSE](LICENSE).
