# Installing VEX

VEX release archives include the command-line compiler and a matching Zig toolchain. VEX uses Zig's bundled Clang/LLVM driver for its default native build path, so a separate C or LLVM installation is not required for ordinary VEX programs.

## 1. Install VEX

### Windows

Open PowerShell and download the installer script:

```powershell
Invoke-WebRequest -Uri https://github.com/Atiqxyz/vex/releases/latest/download/install.ps1 -OutFile install-vex.ps1
.\install-vex.ps1
```

The script verifies the downloaded archive against the release SHA-256 manifest, installs VEX for your Windows user, adds VEX to your user `PATH`, and installs the VS Code extension when the `code` command is available.

### Linux and macOS

Run:

```sh
curl -fsSL https://github.com/Atiqxyz/vex/releases/latest/download/install.sh -o install-vex.sh
bash install-vex.sh
```

The installer selects the matching package for Linux x64, Intel macOS, or Apple Silicon macOS; verifies its SHA-256 digest; installs VEX under `~/.local/opt/vex`; and adds `VEX_HOME` and the VEX executable directory to your shell startup files. When the `code` command is available, it also installs the bundled VS Code extension.

Open a new terminal after installation, or reload your shell profile. Check the setup with:

```sh
vex --version
```

### Manual installation

Portable archives are also published for users who prefer not to run an install script:

- Windows x64: `vex-windows-x64.zip`
- Linux x64: `vex-linux-x64.tar.gz`
- macOS Intel x64: `vex-macos-x64.tar.gz`
- macOS Apple Silicon ARM64: `vex-macos-arm64.tar.gz`

Extract the archive and keep the `bin` and `toolchain` directories together. Add the `bin` directory to `PATH`. The VS Code extension is included at `share/vex/vex-language-0.1.0.vsix`; install it from VS Code's **Install from VSIX...** command if the installer could not use the `code` command.

## 2. VS Code integration

The installed extension recognizes `.vex` files and provides Check, Generate C Preview, Build, and Run commands. Open a folder in VS Code, create a `hello.vex` file, and use **VEX: Run Current File**. The extension uses VEX's default LLVM native backend.

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

The release workflow builds and tests the CLI on Windows x64, Linux x64, Intel macOS, and Apple Silicon macOS. It downloads the pinned Zig 0.17.1 toolchain for each target, verifies its SHA-256 digest, stages it with VEX, and bundles the VS Code extension. Version tags publish portable archives, installer scripts, the VSIX, and a SHA-256 manifest to GitHub Releases.

The current installer scripts provide per-user setup on Windows, Linux, and macOS. Native MSI/EXE, macOS PKG, and Linux DEB/RPM installers are not part of this release yet.
