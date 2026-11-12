# VEX Language Support for Visual Studio Code

This folder contains the first-party VS Code language extension for VEX.

## Current support

- Recognizes `.vex` files as VEX source.
- Highlights the syntax defined by the VEX language specification.
- Supports line and block comments.
- Configures bracket matching, auto-closing, surrounding pairs, indentation, and region folding.
- Provides starter snippets for functions, bindings, conditionals, loops, output, and multiline expressions.

This is the initial declarative editor-support milestone. It does not yet provide compiler diagnostics, autocomplete based on program semantics, Build/Run commands, or debugging. Those require compiler/toolchain integration and, for richer language intelligence, a VEX language server.

The extension intentionally follows the VEX specification in the repository. It does not reuse syntax or implementation from unrelated languages named VEX.

## Try the extension locally

1. Install Visual Studio Code.
2. Open this `editors/vscode` folder as a workspace.
3. Press `F5` to launch an Extension Development Host.
4. In the new window, create or open a `.vex` file.

No npm dependencies are required for this declarative first milestone.

## Packaging

To create a VSIX package for local installation or distribution, install the VS Code Extension Manager (`@vscode/vsce`) and run its package command from this directory. Marketplace publishing is a separate release step and is not implied by this source extension.

## Related project files

- [VEX language specification](../../SPEC.md)
- [VEX compiler README](../../README.md)
