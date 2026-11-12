# VEX Language Support for Visual Studio Code

This folder contains the first-party VS Code language extension for VEX.

## Current support

- Recognizes .vex files as VEX source.
- Highlights syntax defined by the VEX language specification.
- Supports line and block comments.
- Configures bracket matching, auto-closing, surrounding pairs, indentation, and region folding.
- Provides starter snippets for functions, bindings, conditionals, loops, output, and multiline expressions.
- Provides VEX: Check Current File, VEX: Generate C Preview, VEX: Build Current File, and VEX: Run Current File.
- Displays compiler output in the VEX output channel.

Compiler diagnostics in the Problems panel, semantic autocomplete, debugging, and a language server are not implemented yet.

The extension follows the VEX specification in this repository. It does not reuse syntax or implementation from unrelated projects named VEX.

## Install a packaged extension

Download the vex-language-<version>.vsix asset from the [VEX GitHub Releases page](https://github.com/Atiqxyz/vex/releases). In VS Code, open the Extensions view, select its … menu, choose Install from VSIX…, and select the downloaded file.

Install the matching VEX CLI archive from the same release. Add its bin directory to PATH, or set the vex.compilerPath VS Code setting to the executable's full path. The CLI build and run commands also require a C11 compiler; see [the installation guide](../../docs/install.md).

## Try the extension locally

1. Install Visual Studio Code.
2. Open this editors/vscode folder as a workspace.
3. Press F5 to launch an Extension Development Host.
4. In the new window, open a .vex file.
5. If the VEX compiler executable is not on your PATH, set vex.compilerPath in the Extension Development Host settings to its full path.
6. Use the Check, C Preview, Build, and Run actions in the editor title bar or Command Palette.

The extension uses only Node.js built-ins and the VS Code API at runtime.

## Packaging

From this directory, invoke the VS Code Extension Manager (@vscode/vsce):

~~~sh
npx --yes @vscode/vsce package --no-dependencies
~~~

This produces a VSIX package for local installation or distribution. The release workflow builds the VSIX automatically. Marketplace publishing is a separate release step and is not performed automatically.

## Related project files

- [VEX language specification](../../SPEC.md)
- [VEX compiler README](../../README.md)
