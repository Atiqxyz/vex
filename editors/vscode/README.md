# VEX Language Support for Visual Studio Code

This folder contains the first-party VS Code language extension for VEX.

## Current support

- Recognizes `.vex` files as VEX source.
- Highlights syntax defined by the VEX language specification.
- Supports line and block comments.
- Configures bracket matching, auto-closing, surrounding pairs, indentation, and region folding.
- Provides starter snippets for functions, bindings, conditionals, loops, output, and multiline expressions.
- Adds editor actions for **VEX: Check Current File** and **VEX: Generate C Preview**, using the existing compiler commands.

This is the first editor-integration milestone. The extension does not yet provide Build/Run, compiler diagnostics in the Problems panel, semantic autocomplete, or debugging. Build/Run will follow once the compiler can produce and execute native executables. Rich language intelligence will follow through a VEX language server.

The extension follows the VEX specification in this repository. It does not reuse syntax or implementation from unrelated projects named VEX.

## Try the extension locally

1. Install Visual Studio Code.
2. Open this `editors/vscode` folder as a workspace.
3. Press `F5` to launch an Extension Development Host.
4. In the new window, open a `.vex` file.
5. If the VEX compiler executable is not on your PATH, set `vex.compilerPath` in the Extension Development Host settings to its full path.
6. Use the check and C-preview actions in the editor title bar or Command Palette.

The extension uses only Node.js built-ins and the VS Code API; no npm dependencies are required.

## Packaging

To create a VSIX package for local installation or distribution, install the VS Code Extension Manager (`@vscode/vsce`) and run its package command from this directory. Marketplace publishing is a separate release step.

## Related project files

- [VEX language specification](../../SPEC.md)
- [VEX compiler README](../../README.md)
