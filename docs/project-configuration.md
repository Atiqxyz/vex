# Project configuration

VEX uses a small `vex.toml` file to describe a local project. It connects the project's entry source, module graph, native backend, and executable output without requiring a dependency manager.

## Create a project

Run:

~~~sh
vex init my_app
~~~

This creates `vex.toml` and `src/main.vex` in the current directory. It refuses to overwrite either file.

## Manifest format

~~~toml
[project]
name = "my_app"
entry = "src/main.vex"
backend = "llvm"
output = "build/my_app"
~~~

The supported settings are:

| Setting | Default | Meaning |
|---|---|---|
| `name` | Project directory name | Local project label |
| `entry` | `src/main.vex` | Root VEX source file |
| `backend` | `llvm` | Native backend: `llvm` or `c` |
| `output` | `build/<name>` | Executable path relative to the project |

Values are quoted strings inside one `[project]` section. The parser rejects duplicate keys, unknown keys, extra sections, malformed strings, absolute paths, and paths containing `..`. The entry must exist and be a regular file. Project builds create the output directory automatically. On Windows, `.exe` is added when the output has no extension.

## Commands

Run these from the project directory or a subdirectory:

~~~sh
vex check
vex ir
vex build
vex run
~~~

When no source paths are supplied, VEX searches the current directory and its parents for the nearest `vex.toml`. The configured entry is then passed through the ordinary recursive import resolver, semantic checker, IR verifier, and selected backend.

The `backend` setting controls `vex build` and `vex run`. Use `vex build-c` / `vex run-c` or `vex build-llvm` / `vex run-llvm` to override it explicitly. The `-o` argument overrides the configured output path for build commands.

Commands that receive one or more explicit source files retain their existing behavior and do not require a manifest.

## Current boundary

This is a local project configuration, not a package manager. It does not resolve external dependencies, pin versions, create lockfiles, or fetch registries. All imported VEX modules are still compiled together into one typed IR program and linked into one native executable. Cached separate-object compilation and independent object linking remain future work.
