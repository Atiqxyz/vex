# VEX

**Made for fun. Built to run.**

VEX is an experimental, general-purpose systems programming language. It aims to combine readable syntax with static typing, low-level capabilities, and performance-oriented native compilation.

VEX is an independent project created for experimentation, learning, and practical use. It is not intended to replace existing programming languages.

## Project status

VEX is in its early design and implementation stage. The current C++20 development build includes a lexer and token-inspection command. Parsing, type checking, code generation, and native program compilation are not implemented yet. Language features and syntax may change.

## Design goals

- Readable, consistent syntax
- Static typing with type inference
- Hybrid memory-management model
- Native compilation with low runtime overhead
- Cross-platform compiler and developer tooling
- A compiler architecture that can eventually support self-hosting

These are project goals, not claims that all features are currently available.

## Current development build

### Requirements

- CMake 3.20 or newer
- A C++20 compiler (GCC, Clang, or MSVC)

### Build

```sh
cmake -S . -B build
cmake --build build --config Release
```

### Run the lexer

On Windows with the Visual Studio CMake generator (PowerShell):

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

The lexer prints the token stream with source locations. This is a compiler-development command, not yet a way to execute VEX programs.

### Run tests

```sh
ctest --test-dir build -C Release --output-on-failure
```

## Example of the intended syntax

```vex
make main() {
    let message = "Hello, VEX!"
    say(message)
}
```

This example illustrates the current syntax direction; it is not yet executable in the current build.

## Documentation

- [Working language specification](SPEC.md)
- [Compiler architecture](docs/architecture.md)

## Contributing

Bug reports and design discussions are welcome. Please open an issue for proposed language changes so design decisions can be discussed before implementation.

## License

VEX is distributed under the MIT License. See [LICENSE](LICENSE).
