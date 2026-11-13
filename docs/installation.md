# Installing VEX

VEX release archives include the command-line compiler and a matching Zig toolchain. VEX uses Zig's bundled Clang/LLVM driver for its default native build path, so a separate C or LLVM installation is not required for ordinary VEX programs.

## 1. Download and extract VEX

Open the [VEX Releases page](https://github.com/Atiqxyz/vex/releases) and download the archive matching your operating system:

- Windows x64: `vex-windows-x64.zip`
- Linux x64: `vex-linux-x64.zip`
- macOS Intel x64: `vex-macos-x64.zip`
- macOS Apple Silicon ARM64: `vex-macos-arm64.zip`

Extract the complete `vex` directory to a stable location. **Keep its `bin` and `toolchain` directories together**; VEX discovers the bundled compiler relative to its own executable.

### Windows

Extract the package to a location such as `C:\Tools\VEX`. Add `C:\Tools\VEX\bin` to your user or system `PATH`. Open a new terminal or restart VS Code after changing PATH.

### Linux

Extract the package to a stable location, such as `~/.local/opt/vex`, and add `~/.local/opt/vex/bin` to your shell's `PATH`.

### macOS

Extract the package to a stable location, such as `~/Applications/VEX`, and add `~/Applications/VEX/bin` to your shell's `PATH`.

Verify the installation by running `vex --version`.

## 2. Install the VS Code extension

1. Download `vex-language-0.1.0.vsix` from the same release.
2. Open Visual Studio Code and open the Extensions view.
3. Open the Extensions view's `...` menu and select **Install from VSIX...**.
4. Select the downloaded VSIX file and reload VS Code if prompted.

The extension recognizes `.vex` files and provides Check, Generate C Preview, Build, and Run commands. Ensure the VEX `bin` directory is on PATH, or set the VS Code setting `vex.compilerPath` to the full path of the VEX executable.

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

Program output appears in the **VEX** output channel. The editor's Build and Run actions use VEX's default LLVM native backend.

## 4. Compiler backends

The default commands use VEX's LLVM backend:

- `vex build file.vex` builds a native executable.
- `vex run file.vex` builds and executes a program.
- `vex emit-llvm file.vex` prints LLVM IR.

The release package includes Zig 0.17.1, whose Clang/LLVM toolchain is used to compile the generated LLVM IR. You do not need to install a separate C compiler or LLVM for ordinary VEX development.

Advanced users can override toolchain discovery with `VEX_LLVM_ZIG` (path to Zig) or `VEX_LLVM_CC` / `LLVM_CC` (path to a Clang driver). `VEX_HOME` can point to the package root when VEX is launched through a wrapper or custom layout.

The C backend remains available as an explicit fallback:

- `vex emit-c file.vex`
- `vex build-c file.vex`
- `vex run-c file.vex`

Those `-c` commands require an installed C11 compiler. Set `VEX_CC` or `CC` to select it when automatic discovery is insufficient.

## 5. Portable toolchain layout for packagers

CMake can stage a supplied Zig installation alongside VEX:

```sh
cmake -S . -B build -DVEX_ZIG_TOOLCHAIN_DIR=/path/to/zig-installation
cmake --build build --config Release
cmake --install build --prefix dist/vex
```

The supplied directory must contain the `zig` executable (or `zig.exe` on Windows) at its root and its complete matching `lib/` directory. VEX expects:

```text
vex/
  bin/
    vex
  toolchain/
    zig
    lib/
    LICENSE
    ...
```

The complete upstream Zig directory is staged so its compiler libraries and accompanying license notices remain together. The CMake option `VEX_LLVM_TOOLCHAIN_DIR` remains available for packagers who provide a conventional LLVM installation containing `bin/clang`.

## Troubleshooting

- **VS Code cannot start VEX:** confirm that the VEX `bin` directory is on PATH, restart VS Code, or set `vex.compilerPath` to the full executable path.
- **VEX reports that its bundled Zig toolchain is missing:** restore the complete package layout; do not move `bin/vex` away from its sibling `toolchain` directory.
- **The LLVM build fails on a particular platform:** check the diagnostic in the VEX output channel. The default path uses the bundled Clang/LLVM driver; the C fallback is available with `build-c` and `run-c`.
- **The C fallback cannot find a compiler:** install a C11 compiler and ensure it is on PATH, or set `VEX_CC`.
- **The VS Code extension does not activate:** confirm that the file extension is `.vex` and reload the VS Code window.

## Distribution status

The release workflow builds and tests the CLI on Windows x64, Linux x64, Intel macOS, and Apple Silicon macOS, downloads the pinned Zig 0.17.1 toolchain for each target, verifies each archive's SHA-256 digest, stages it with VEX, and packages the VS Code extension. Version tags publish the resulting platform archives and VSIX to GitHub Releases.

The current delivery is a portable extracted package rather than a native Windows installer, macOS package installer, or Linux system package. Those native installers and automatic VS Code extension setup are a later installation milestone.
