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
- an initial C11 source backend and an initial textual LLVM IR backend for the current IR instruction set, including functions, control flow, primitive values, the `say` output intrinsic, and mathematical intrinsics (`sqrt`, `sin`, `cos`, `pow`, and `abs`);
- a command-line interface for token inspection, AST printing, semantic checking, IR inspection, C source emission, native builds, and program execution;
- cross-platform process launching and temporary-build management for the bootstrap toolchain;
- unit tests plus a cross-platform integration test that builds and executes VEX programs through the public CLI.

VEX can emit portable C11 and LLVM IR. The C `build`/`run` commands use an installed C11 compiler; the LLVM `build-llvm`/`run-llvm` commands use an installed Clang driver to optimize LLVM IR and generate native executables without generating C. The LLVM toolchain is not bundled yet, and the compiler is not self-hosted. Collection types, a complete runtime, and the memory model are not implemented.

## Design direction

- Static typing with inferred local binding types.
- Newlines separate statements; semicolons are not used.
- Curly braces delimit blocks; indentation is not semantic.
- Function calls use parentheses.
- A hybrid memory-management model is the selected direction, but ownership, borrowing, and allocation semantics remain open.
- The C backend is a bootstrap target; the IR and frontend remain independent of it. A native backend is a future goal.

These are design goals, not claims that every feature already exists.

## Install prebuilt releases

Prebuilt command-line binaries for Windows, Linux, and macOS, plus the VS Code extension package, are published on the [GitHub Releases page](https://github.com/Atiqxyz/vex/releases). See [the installation guide](docs/installation.md) for setup instructions.

The command-line binary does not bundle C or LLVM compiler toolchains. Frontend commands and `emit-c`/`emit-llvm` work without one; `build`/`run` require a C11 compiler, while `build-llvm`/`run-llvm` require Clang with LLVM IR support. Set `VEX_CC` or `VEX_LLVM_CC` to choose the corresponding compiler executable.

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
./build/vex emit-llvm examples/hello.vex > hello.ll
./build/vex build examples/hello.vex
./build/vex build examples/hello.vex -o hello
./build/vex run examples/hello.vex
./build/vex build-llvm examples/hello.vex
./build/vex run-llvm examples/hello.vex
~~~

- `lex` prints token kinds and source positions.
- `parse` prints the parsed abstract syntax tree.
- `check` resolves identifiers and validates the supported static type rules.
- `ir` lowers a semantically valid program into typed SSA IR, verifies it, and prints the result.
- `emit-c` runs the frontend, lowers and verifies IR, then emits C11 source to standard output.
- `build` compiles a VEX source file into a native executable. By default the executable is written beside the source file; use `-o` to choose another path.
- `run` builds through the C backend in a temporary directory, executes it, and removes the temporary build artifacts.
- `emit-llvm` emits textual LLVM IR from verified VEX IR.
- `build-llvm` uses Clang and LLVM optimization/code generation to build a native executable without generating C.
- `run-llvm` builds through the LLVM backend in a temporary directory and executes the native program.

The C `build` and `run` commands use a C11 compiler as the bootstrap backend. The LLVM build/run commands use Clang as the native code-generation driver. Neither compiler toolchain is bundled in the current release packages.

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
- [Standard library status](docs/standard-library.md)
- [Compiler design review and references](docs/compiler-design-review.md)

## License

VEX is distributed under the MIT License. See [LICENSE](LICENSE).
