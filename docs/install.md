# Installing VEX

This guide covers the prebuilt CLI archives and the VS Code extension published with a VEX GitHub Release.

## 1. Download VEX

Open the [VEX Releases page](https://github.com/Atiqxyz/vex/releases/latest) and download:

- **vex-cli-windows-x86_64.zip** for 64-bit Windows;
- **vex-cli-linux-x86_64.zip** for 64-bit Linux;
- **vex-cli-macos-x86_64.zip** for Intel-based Macs;
- **vex-language-<version>.vsix** for the VS Code extension.

The CLI archives contain the **bin** directory with the VEX executable and **share/doc/vex** with the license and documentation.

## 2. Make the VEX executable available

Extract the CLI archive to a folder you control. Add its **bin** directory to your user PATH, then restart VS Code so it inherits the updated environment.

You can also skip changing PATH: open VS Code Settings, search for **VEX Compiler Path**, and set **vex.compilerPath** to the full path of the extracted executable (vex.exe on Windows, vex on Linux or macOS).

Verify the installation from a terminal only if you want to:

~~~text
vex --version
~~~

## 3. Install the VS Code extension

1. Open VS Code.
2. Open the Extensions view.
3. Select the **…** menu in the Extensions view.
4. Choose **Install from VSIX…**.
5. Select the **vex-language-<version>.vsix** file from the release assets.
6. Open or create a file ending in **.vex**.

The extension provides **VEX: Check Current File**, **VEX: Generate C Preview**, **VEX: Build Current File**, and **VEX: Run Current File**. These commands are available from the Command Palette; Build and Run are also available in the editor title bar.

## 4. Install a C11 compiler for Build and Run

The VEX CLI package does not include a C compiler. VEX's current executable-generation backend emits C11 and invokes a system C compiler.

- **Windows:** install a C11-capable compiler such as LLVM/Clang or GCC and make its executable available on PATH. For MSVC, run VS Code from a Visual Studio Developer environment so cl.exe and its SDK are available. You may set the VEX_CC environment variable to the full path of a compiler executable.
- **Linux:** install GCC or Clang using your distribution's package manager. The executable is normally cc, gcc, or clang.
- **macOS:** install the Xcode Command Line Tools, which provide Clang.

The compiler frontend commands (lex, parse, check, and ir) and C preview (emit-c) do not require a C compiler. Only build and run need one.

VEX_CC is a path to one compiler executable; it is not a shell command with additional flags.

## 5. Run your first program

Create hello.vex, write a program supported by the current VEX language subset, open it in VS Code, and select **VEX: Run Current File**.

Build and Run output appears in the VEX output channel. Build writes the executable beside the source file; Run uses a temporary build directory and removes its temporary files after execution.

## Troubleshooting

- **Could not start the VEX compiler:** check vex.compilerPath or ensure the VEX bin directory is on PATH, then restart VS Code.
- **No usable C11 compiler was found:** install a compiler and make it available on PATH, or set VEX_CC to its executable path.
- **C compiler exited with an error:** verify the C compiler's SDK/development environment is installed. On Windows, MSVC generally needs a Developer environment.
- **The source file is not recognized:** confirm the filename ends in .vex and reload VS Code after installing the extension.

## Scope and platform notes

The initial release pipeline targets x86-64 Windows, x86-64 Linux, and Intel x86-64 macOS. ARM64 CLI builds and automatic Marketplace publishing are not part of this release pipeline yet. The extension is distributed as a VSIX for manual installation.
