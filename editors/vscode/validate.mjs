import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import path from "node:path";

const root = path.dirname(fileURLToPath(import.meta.url));
const readJson = async (relativePath) =>
  JSON.parse(await readFile(path.join(root, relativePath), "utf8"));

const manifest = await readJson("package.json");
const language = await readJson("language-configuration.json");
const grammar = await readJson("syntaxes/vex.tmLanguage.json");
const snippets = await readJson("snippets/vex.json");

assert.equal(manifest.main, "./extension.js");
assert.ok(manifest.contributes.languages.some((entry) =>
  entry.id === "vex" && entry.extensions.includes(".vex")
));
assert.ok(manifest.contributes.grammars.some((entry) =>
  entry.language === "vex" &&
  entry.scopeName === grammar.scopeName &&
  entry.path === "./syntaxes/vex.tmLanguage.json"
));
assert.ok(manifest.contributes.snippets.some((entry) =>
  entry.language === "vex" && entry.path === "./snippets/vex.json"
));
assert.ok(language.comments.lineComment === "//");
assert.ok(language.brackets.some(([open, close]) => open === "{" && close === "}"));
assert.ok(grammar.patterns.length > 0);
assert.ok(snippets["Main function"]);
assert.ok(snippets["Function"]);
assert.ok(manifest.contributes.commands.some((entry) => entry.command === "vex.checkCurrentFile"));
assert.ok(manifest.contributes.commands.some((entry) => entry.command === "vex.emitCPreview"));
assert.ok(manifest.contributes.commands.some((entry) => entry.command === "vex.buildCurrentFile"));
assert.ok(manifest.contributes.commands.some((entry) => entry.command === "vex.runCurrentFile"));

console.log("VEX VS Code extension metadata and language assets are valid.");
