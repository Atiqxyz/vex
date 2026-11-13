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
- fixed-size, one-dimensional arrays with inferred primitive element types, indexing, and indexed updates;
- `for-in` iteration over arrays and integer ranges using `start..end` (exclusive) or `start..=end` (inclusive);
- C11 and textual LLVM IR backends for functions, control flow, primitive values, checked fixed-array access, the `say` output intrinsic, and mathematical intrinsics (`sqrt`, `sin`, `cos`, `pow`, and `abs`);
- a command-line interface for token inspection, AST printing, semantic checking, IR inspection, C source emission, LLVM IR emission, native builds, and program execution;
- cross-platform process launching and temporary-build management;
- unit tests plus cross-platform integration tests for the public CLI and portable compiler discovery.

The default `build` and `run` commands use the LLVM backend and a Clang driver. Release packages include a pinned Zig toolchain, which supplies the bundled Clang/LLVM driver. This removes the need for users to separately install a C compiler or LLVM for ordinary VEX development. The optional `build-c` and `run-c` commands retain the C11 bootstrap backend for compatibility and fallback use.

VEX is still an early language implementation. Dynamic collections, structures, a complete runtime, modules, and the memory model are not implemented yet. Fixed arrays are value aggregates; their element type and length are inferred from array literals.

## Design direction

- Static typing with inferred local binding types.
- Newlines separate statements; semicolons are not used.
- Curly braces delimit blocks; indentation is not semantic.
- Function calls use parentheses.
- A hybrid memory-management model is the selected direction, but ownership, borrowing, and allocation semantics remain open.
- The C backend remains an optional bootstrap target; the IR and frontend are independent of it. LLVM is the default native compilation path.

These are design goals, not claims that every feature already exists.

## Install prebuilt releases

Install VEX with the Windows PowerShell script or the Linux/macOS shell script from the [GitHub Releases page](https://github.com/Atiqxyz/vex/releases). The scripts verify the archive, install the bundled Zig 0.17.1 toolchain, configure the user PATH, and install the VS Code extension when its command-line launcher is available. Portable archives are also available for manual installation.

See [the installation guide](docs/installation.md) for complete steps. Native MSI/EXE, macOS PKG, and Linux DEB/RPM packages are not available yet.

## Build from source

Requirements: CMake 3.20+ and a C++20 compiler (GCC, Clang, or MSVC). A C11 compiler is optional and is only required to use the `build-c` and `run-c` fallback commands.

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build --config Release --output-on-failure
```

## Compiler commands

On Windows with a multi-configuration CMake generator:

```powershell
.\build\Release\vex.exe --help
.\build\Release\vex.exe --version
.\build\Release\vex.exe lex examples\hello.vex
.\build\Release\vex.exe parse examples\hello.vex
.\build\Release\vex.exe check examples\hello.vex
.\build\Release\vex.exe ir examples\hello.vex
.\build\Release\vex.exe emit-llvm examples\hello.vex
```

On Linux or macOS:

```sh
./build/vex --help
./build/vex --version
./build/vex lex examples/hello.vex
./build/vex parse examples/hello.vex
./build/vex check examples/hello.vex
./build/vex ir examples/hello.vex
./build/vex emit-c examples/hello.vex > hello.c
./build/vex emit-llvm examples/hello.vex > hello.ll
./build/vex build examples/hello.vex
./build/vex run examples/hello.vex
./build/vex build-c examples/hello.vex
./build/vex run-c examples/hello.vex
```

- `lex` prints token kinds and source positions.
- `parse` prints the parsed abstract syntax tree.
- `check` resolves identifiers and validates the supported static type rules.
- `ir` lowers a semantically valid program into typed SSA IR, verifies it, and prints the result.
- `emit-c` emits C11 source; `emit-llvm` emits textual LLVM IR.
- `build` and `run` compile through the LLVM backend and bundled Zig/Clang toolchain.
- `build-llvm` and `run-llvm` are explicit aliases for the default LLVM path.
- `build-c` and `run-c` use the optional C11 bootstrap backend.

The compiler itself remains implemented in C++. VEX does not need to be self-hosted to become a general-purpose language.

## Example syntax direction

```vex
make main() {
    let message = "Hello, VEX!"
    say(message)
}
```

The current compiler supports a small set of primitive types, functions, control flow, strings, output, arithmetic, comparisons, array iteration, integer-range iteration, and math intrinsics. The runtime and language features are still under active development.

## Project documents

- [Working language specification](SPEC.md)
- [Compiler architecture](docs/architecture.md)
- [Standard library status](docs/standard-library.md)
- [Compiler design review and references](docs/compiler-design-review.md)

## License

VEX is distributed under the MIT License. See [LICENSE](LICENSE). The portable package also retains the upstream Zig toolchain distribution files and license notices in its `toolchain/` directory.
