# Modules and import resolution

VEX supports source-level modules built from recursively imported files. Each imported file has its own module identity and namespace; the compiler resolves the module graph and lowers the program into one linked IR module.

## Import syntax

Imports are top-level declarations and use a quoted path relative to the file containing the import:

~~~vex
import "math/geometry.vex" as geometry
import "math/numerics.vex"
~~~

An explicit alias is recommended. Without `as`, the file name without its extension becomes the module alias, so the second import above is available as `numerics`. The alias must be a valid VEX identifier. A module may be imported more than once, including under more than one alias. Reusing one alias for two different modules is an error.

Absolute paths are rejected. String-literal escapes are decoded before filesystem lookup. Canonical paths identify modules, repeated imports are deduplicated, and import cycles terminate without loading a file more than once. Diagnostics retain the original path and line of each source file.

## Public and private declarations

Declarations are private by default. A module exposes selected functions and nominal types by marking them `pub`:

~~~vex
pub make distance(point: Point) -> float {
    return sqrt(point.x * point.x + point.y * point.y)
}

pub struct Point {
    x: float,
    y: float,
}

pub enum ResultState {
    Ready,
    Failed,
}
~~~

An importing module accesses exported functions and types through its alias:

~~~vex
import "geometry.vex" as geometry

make main() {
    let point: geometry.Point = geometry.Point { x: 3.0, y: 4.0 }
    say(geometry.distance(point))
}
~~~

The same qualification rule applies to enum constructors and match patterns, for example `geometry.ResultState.Ready`. A module cannot access another module's private functions or types. Within a module, private declarations remain available to that module's own functions.

Only functions, structures, and enums are exportable in this module-system version. A public function signature, public structure field, or public enum payload cannot expose a private nominal type from the declaring module; make that type public or keep the containing declaration private. Module-level executable statements are not permitted in imported files; put executable code inside functions. The root module keeps the existing implicit top-level program behavior when it does not define an explicit `main`.

## Module graph and name identity

The loader canonicalizes paths, visits every module once, and loads dependencies before their importers. The semantic analyzer collects declarations across the complete graph before checking function bodies, so cross-file and cyclic function references can resolve. Each module has independent function, structure, and enum namespaces. Two modules may define the same local name without colliding.

The lowerer assigns imported declarations deterministic module-qualified linker names. This prevents same-named functions and nominal types in different modules from colliding in C11 or LLVM output. Explicit source files supplied together on the command line remain grouped into one root module for backwards compatibility; recursively imported files form separate modules.

A module alias must be unique within the importing module unless repeated imports with that alias resolve to the same canonical module. If two different modules need similar aliases, use explicit aliases to disambiguate them.

## Compilation boundary

The `check`, `ir`, `emit-c`, `emit-llvm`, `build`, `build-c`, `build-llvm`, `run`, `run-c`, and `run-llvm` commands use the import-aware source-bundle loader. The standalone `lex` and `parse` commands inspect only the file explicitly named on the command line.

All modules in a program are currently compiled together into one typed IR module and linked into one native executable. Independent cached object compilation, module interfaces, package/dependency management, re-exports, and dynamic library packaging are not part of this version. A lightweight `vex.toml` project manifest configures the local entry file, backend, and executable output; it does not resolve external dependencies. This is a real source-level module and visibility system, but not yet an incremental separate-compilation or package ecosystem.

## Regression coverage

The integration suite covers nested relative imports, repeated imports, import cycles, alias resolution, separate namespaces for identically named functions, exported and private functions/types, qualified structure and enum use, alias conflicts, source-located diagnostics, duplicate explicit roots, and native execution through the C11 and LLVM backends.


## Project configuration

A project manifest gives the module graph an explicit entry point and native build target. Run `vex init [name]` to create a starter `vex.toml` and `src/main.vex`. Project commands invoked without source paths search the current directory and its parents for the nearest manifest, then compile its configured entry and all recursively imported modules.

The supported manifest subset is:

~~~toml
[project]
name = "geometry_app"
entry = "src/main.vex"
backend = "llvm"
output = "build/geometry_app"
~~~

`name` defaults to the project directory name, `entry` defaults to `src/main.vex`, `backend` defaults to `llvm`, and `output` defaults to `build/<name>`. Entry and output paths are relative to the manifest and cannot contain `..` path components or be absolute. The backend is either `llvm` or `c`. Output directories are created by project builds. `build-c` / `run-c` and `build-llvm` / `run-llvm` can explicitly override the configured backend.

The manifest parser intentionally supports one `[project]` table and the four quoted string settings shown above. Duplicate and unknown keys, unknown sections, invalid paths, and unsupported backend values are errors. This provides reproducible local project entry/backend/output selection, but is not yet a package manifest: dependency declarations, version resolution, registries, lockfiles, and cached independent object compilation remain outside this implementation.
