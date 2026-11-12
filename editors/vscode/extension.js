const vscode = require("vscode");
const { spawn } = require("node:child_process");

function activate(context) {
  const output = vscode.window.createOutputChannel("VEX");
  context.subscriptions.push(output);

  async function runCompilerCommand(command, document) {
    if (!document || document.languageId !== "vex") {
      vscode.window.showWarningMessage("Open a VEX source file first.");
      return;
    }

    if (document.isDirty) {
      const saved = await document.save();
      if (!saved) {
        vscode.window.showWarningMessage("Save the VEX file before running the compiler.");
        return;
      }
    }

    const compilerPath = vscode.workspace.getConfiguration("vex").get("compilerPath", "vex");
    const cwd = vscode.workspace.getWorkspaceFolder(document.uri)?.uri.fsPath;
    output.show(true);
    output.appendLine(`> ${compilerPath} ${command} "${document.uri.fsPath}"`);

    return new Promise((resolve) => {
      const child = spawn(compilerPath, [command, document.uri.fsPath], {
        cwd,
        windowsHide: true,
        shell: false
      });

      let stdout = "";
      let stderr = "";
      child.stdout.setEncoding("utf8");
      child.stderr.setEncoding("utf8");
      child.stdout.on("data", (chunk) => { stdout += chunk; });
      child.stderr.on("data", (chunk) => { stderr += chunk; });

      child.on("error", (error) => {
        output.appendLine(`Failed to start VEX compiler: ${error.message}`);
        vscode.window.showErrorMessage("Could not start the VEX compiler. Check vex.compilerPath.");
        resolve({ code: null, stdout, stderr: error.message });
      });

      child.on("close", (code) => {
        if (stdout) output.append(stdout);
        if (stderr) output.append(stderr);
        output.appendLine(`VEX process exited with code ${code}.`);
        resolve({ code, stdout, stderr });
      });
    });
  }

  context.subscriptions.push(
    vscode.commands.registerTextEditorCommand("vex.checkCurrentFile", async (editor) => {
      const result = await runCompilerCommand("check", editor.document);
      if (!result) return;
      if (result.code === 0) {
        vscode.window.showInformationMessage("VEX semantic check passed.");
      } else if (result.code !== null) {
        vscode.window.showErrorMessage("VEX semantic check failed. See the VEX output channel.");
      }
    })
  );

  context.subscriptions.push(
    vscode.commands.registerTextEditorCommand("vex.emitCPreview", async (editor) => {
      const result = await runCompilerCommand("emit-c", editor.document);
      if (!result) return;
      if (result.code === 0) {
        const generated = await vscode.workspace.openTextDocument({
          language: "c",
          content: result.stdout
        });
        await vscode.window.showTextDocument(generated, vscode.ViewColumn.Beside, true);
      } else if (result.code !== null) {
        vscode.window.showErrorMessage("VEX could not generate C. See the VEX output channel.");
      }
    })
  );
}

function deactivate() {}

module.exports = { activate, deactivate };
