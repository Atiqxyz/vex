# VEX

**Made for fun. Built to run.**

VEX is an experimental, general-purpose systems programming language. It aims to combine readable syntax with static typing, type inference, low-level capabilities, and performance-oriented native compilation.

VEX is an independent project for experimentation and practical learning. It is not intended to replace established programming languages.

## Status

The current C++20 development build has:

- immutable source buffers, byte-based source spans, and structured diagnostics;
- a lexer, parser, and initial abstract syntax tree;
- a semantic checker that produces typed expression information, resolved bindings, and function signatures;
- a typed SSA intermediate representation with basic-block parameters, a verifier, and AST-to-IR lowering;
- an initial C11 source backend for the complete current IR instruction set, including functions, control flow, primitive values, and the `say` intrinsic;
- a command-line interface for token inspection, AST printing, semantic checking, IR inspection, C source emission, native builds, and program execution;
- cross-platform process launching and temporary-build management for the bootstrap toolchain;
- unit tests plus a cross-platform integration test that builds and executes VEX programs through the public CLI.

VEX can emit portable C11, build native executables through an installed C11 compiler, and run VEX source files through temporary native builds. The current native toolchain is a bootstrap path through C11, not a self-hosted compiler or a direct native-code backend. Collection types, a complete runtime, and the memory model are not implemented.

## Design direction

- Static typing with inferred local binding types.
- Newlines separate statements; semicolons are not used.
- Curly braces delimit blocks; indentation is not semantic.
- Function calls use parentheses.
- A hybrid memory-management model is the selected direction, but ownership, borrowing, and allocation semantics remain open.
- The C backend is a bootstrap target; the IR and frontend remain independent of it. A native backend is a future goal.

These are design goals, not claims that every feature already exists.

## Build

Requirements: CMake 3.20+ and a C++20 compiler (GCC, Clang, or MSVC). A C11 compiler is required for `build` and `run`; CMake detects and records the configured C compiler when available. You can override compiler discovery with the `VEX_CC` environment variable. The frontend-only commands and `emit-c` do not invoke a C compiler.

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
.\build\Release\vex.exe ir examples\hello.vex
.\build\Release\vex.exe emit-c examples\hello.vex
~~~

On Linux or macOS:

~~~sh
./build/vex --help
./build/vex --version
./build/vex lex examples/hello.vex
./build/vex parse examples/hello.vex
./build/vex check examples/hello.vex
./build/vex ir examples/hello.vex
./build/vex emit-c examples/hello.vex > hello.c
./build/vex build examples/hello.vex
./build/vex build examples/hello.vex -o hello
./build/vex run examples/hello.vex
~~~

- `lex` prints token kinds and source positions.
- `parse` prints the parsed abstract syntax tree.
- `check` resolves identifiers and validates the supported static type rules.
- `ir` lowers a semantically valid program into typed SSA IR, verifies it, and prints the result.
- `emit-c` runs the frontend, lowers and verifies IR, then emits C11 source to standard output.
- `build` compiles a VEX source file into a native executable. By default the executable is written beside the source file; use `-o` to choose another path.
- `run` builds a VEX source file in a temporary directory, executes it, forwards its output and exit status, then removes the temporary build artifacts.

The current `build` and `run` commands use a C11 compiler as the bootstrap backend. VEX itself handles the VEX frontend, IR verification, C generation, compiler invocation, and program launching; users do not need to manually generate or compile C.

## Example syntax direction

~~~vex
make main() {
    let message = "Hello, VEX!"
    say(message)
}
~~~

The initial C backend supplies a small output runtime for `int`, `float`, `bool`, and length-aware UTF-8 strings. It is a bootstrap runtime, not yet the complete VEX standard library or finalized memory model.

## Project documents

- [Working language specification](SPEC.md)
- [Compiler architecture](docs/architecture.md)
- [Compiler design review and references](docs/compiler-design-review.md)

## License

VEX is distributed under the MIT License. See [LICENSE](LICENSE).
