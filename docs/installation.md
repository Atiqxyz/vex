# Installing VEX

VEX releases contain the command-line compiler and a VS Code extension package.

## 1. Download the compiler

Open the [VEX Releases page](https://github.com/Atiqxyz/vex/releases) and download the archive matching your operating system:

- Windows x64: `vex-windows-x64.zip`
- Linux x64: `vex-linux-x64.zip`
- macOS Intel x64: `vex-macos-x64.zip`

Extract the archive. The executable is in the `bin` directory.

### Windows

Move the extracted `bin` directory to a stable location, such as `C:\\Tools\\VEX\\bin`, and add that directory to your user or system `PATH`. Open a new terminal or restart VS Code after changing PATH.

### Linux

Move the extracted `bin/vex` executable to a directory on your PATH, for example `~/.local/bin`. Ensure that directory is included in your shell's PATH.

### macOS

Move `bin/vex` to a directory on your PATH, such as `/usr/local/bin` or `~/.local/bin`. On first use, macOS may ask you to confirm that you want to run the downloaded executable.

Verify the installation by running `vex --version` in a terminal. This is an optional verification step; the VS Code commands do not require manually entering compiler commands.

## 2. Install the VS Code extension

1. Download `vex-language-0.1.0.vsix` from the same release.
2. Open Visual Studio Code.
3. Open the Extensions view.
4. Open its `...` menu and select **Install from VSIX...**.
5. Select the downloaded VSIX file.

The extension recognizes `.vex` files and provides Check, Generate C Preview, Build, and Run commands.

## 3. Open and run a VEX program

1. Open a folder in VS Code.
2. Create a file named `hello.vex`.
3. Enter:

```vex
make main() {
    say("Hello, VEX!")
}
```

4. Save the file.
5. Choose **VEX: Run Current File** from the Command Palette or use the Run action in the editor title bar.

Program output appears in the **VEX** output channel.

## 4. C compiler requirement

The VEX frontend and syntax tools do not need a C compiler. The current `build` and `run` commands use the C11 bootstrap backend, so a C compiler must also be installed on the computer:

- Windows: a working MSVC developer environment, Clang, or GCC.
- Linux: GCC or Clang (often available as `cc`).
- macOS: Apple Command Line Tools (Clang).

VEX checks `VEX_CC`, `CC`, its configured compiler fallback, and common compiler executable names. If automatic discovery does not find your compiler, set `VEX_CC` to the compiler executable path. The extension itself can be pointed to the VEX executable with the VS Code setting `vex.compilerPath`.

VEX does not currently bundle a C compiler, and it is not yet a self-hosted compiler or a direct machine-code backend.

## 5. Experimental LLVM backend

The development build also provides `vex emit-llvm`, `vex build-llvm`, and `vex run-llvm`. These commands use the LLVM backend and an installed Clang driver; they do not generate C as an intermediate compilation step. Set `VEX_LLVM_CC` to the Clang executable path when Clang is not on PATH. Current release packages do not bundle Clang or LLVM, so this is not yet a standalone dependency-free installation path.

## Troubleshooting

- **VS Code cannot start VEX:** confirm that `vex` is on PATH, restart VS Code, or set `vex.compilerPath` to the full executable path.
- **Build or Run reports that no C11 compiler was found:** install a C compiler and ensure it is available on PATH, or set `VEX_CC` to its executable path.
- **LLVM build or run reports that Clang was not found:** install Clang with LLVM IR support, or set `VEX_LLVM_CC` to its executable path.
- **MSVC is installed but not found:** launch VS Code from a Visual Studio Developer Command Prompt, or configure an available Clang/GCC compiler.
- **The extension does not activate:** confirm that the file extension is `.vex` and reload the VS Code window.

## Distribution status

The release workflow builds and tests the CLI on Windows, Linux, and macOS, packages the VS Code extension as a VSIX, and attaches the resulting files to GitHub Releases when a version tag is pushed. A GitHub release must exist before these download instructions provide actual release assets.
