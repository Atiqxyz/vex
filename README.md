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
- nominal structures and inline tagged enums, including unit and positional-payload variants, typed constructors, payload projection, and exhaustive statement-form `match` with scoped immutable payload bindings;
- fixed-size, one-dimensional arrays with inferred primitive element types (including unsigned `byte`), indexing, indexed updates, and by-value function parameters/returns;
- a first-class unsigned 8-bit `byte` type (0–255), explicit checked `to_byte(int)` and zero-extending `to_int(byte)` conversions, byte-width bitwise operations, and unsigned byte comparisons;
- mutable dynamic `byte[]` buffers, bounds-checked owning `byte_slice` copies, and checked 16-/32-bit unsigned little-/big-endian binary integer codecs;
- whole-file binary `read_file` and `write_file` operations in both backends, with chunked reads, geometric buffer growth, deterministic file close, and `-1` error reporting;
- heap-backed, one-dimensional dynamic arrays for primitive elements (including `byte`), with typed empty literals, geometric growth, checked indexing, `length`, `push`, and `pop`;
- shared-reference dynamic arrays across aliases, function parameters, and function returns, with `for-in` iteration over fixed/dynamic arrays and integer ranges;
- C11 and textual LLVM IR backends for functions, control flow, primitive values, checked fixed/dynamic-array access, dynamic allocation and cleanup, the `say` output intrinsic, and mathematical intrinsics (`sqrt`, `sin`, `cos`, `pow`, and `abs`);
- a command-line interface for token inspection, AST printing, semantic checking, IR inspection, C source emission, LLVM IR emission, native builds, and program execution;
- multi-file translation units: source files can be passed together to `check`, `ir`, `emit-c`, `emit-llvm`, `build`, and `run`;
- cross-platform process launching and temporary-build management;
- unit tests plus cross-platform integration tests for the public CLI and portable compiler discovery.

Multiple source files are currently combined in command-line order into one translation unit and one global namespace. This is the first multi-file compilation step; import declarations, module namespaces, visibility controls, and separate compilation are not implemented yet.

The binary file helpers operate on complete files rather than exposing streaming handles. `read_file` appends to a mutable byte array and returns the appended byte count or `-1`; `write_file` replaces the destination file and returns the byte count or `-1`. The default `build` and `run` commands use the LLVM backend and a Clang driver. Release packages include a pinned Zig toolchain, which supplies the bundled Clang/LLVM driver. This removes the need for users to separately install a C compiler or LLVM for ordinary VEX development. The optional `emit-c`, `build-c`, and `run-c` commands explicitly select the C11 backend.

VEX is an early language implementation. It supports nominal structures and enums, fixed and dynamic arrays (including supported nested jagged shapes) of primitive and aggregate values, exhaustive enum matching, nested aggregate updates, functions, control flow, and length-aware byte strings. Modules and a general-purpose ownership/borrowing system remain unfinished. Fixed arrays and structures have value semantics; dynamic arrays are heap-backed shared references whose aliases and function parameters/returns refer to the same collection. Dynamic-array and concatenated-string allocations are tracked by the generated program and reclaimed when its entry function returns. This program-lifetime strategy is an initial runtime contract, not the final memory model.

## Design direction

- Static typing with inferred local binding types.
- Newlines separate statements; semicolons are not used.
- Curly braces delimit blocks; indentation is not semantic.
- Function calls use parentheses.
- The wider memory model is still under design. Dynamic arrays use shared heap references and dynamic arrays plus concatenated strings currently use program-lifetime allocation tracking. There is no per-scope deallocation, ownership/borrowing checker, or public allocator API yet.
- VEX is not required to be self-hosted, but its long-term language, runtime, and tooling design must make it feasible to implement the entire compiler in VEX.
- The C backend is an optional, explicitly selected target; the IR and frontend are independent of it. LLVM is the default native compilation path.

These are design goals, not claims that every feature is already implemented.

## Install prebuilt releases

Install VEX with the Windows PowerShell script or the Linux/macOS shell script from the [GitHub Releases page](https://github.com/Atiqxyz/vex/releases). The scripts verify the archive, install the bundled Zig 0.17.1 toolchain, configure the user PATH, and install the VS Code extension when its command-line launcher is available. Portable archives are also available for manual installation.

See [the installation guide](docs/installation.md) for complete steps. Native MSI/EXE, macOS PKG, and Linux DEB/RPM packages are not available yet.

## Build from source

Requirements: CMake 3.20+ and a C++20 compiler (GCC, Clang, or MSVC). A C11 compiler is optional and is only required to use `build-c` and `run-c`.

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
- `build-c` and `run-c` explicitly select the optional C11 backend; `emit-c` emits its portable C source.

The compiler remains implemented in C++. Self-hosting is not a release requirement, but VEX is being designed so its language, runtime, and tooling can eventually support implementing the complete compiler in VEX.

## Example syntax direction

```vex
make main() {
    let message = "Hello, VEX!"
    say(message)
}
```

The current compiler supports primitive types, nominal structures, fixed and dynamic arrays, functions, control flow, strings, arithmetic, comparisons, iteration, compound assignment, basic math intrinsics, and both C and LLVM execution paths. The language, runtime, and standard library remain under active development.

## Project documents

- [Working language specification](SPEC.md)
- [Master roadmap and design map](docs/master-roadmap.md)
- [Compiler architecture](docs/architecture.md)
- [Standard library status](docs/standard-library.md)
- [Compiler design review and references](docs/compiler-design-review.md)
- [Language completeness matrix](docs/language-completeness-matrix.md)

## License

VEX is distributed under the MIT License. See [LICENSE](LICENSE). The portable package also retains the upstream Zig toolchain distribution files and license notices in its `toolchain/` directory.
