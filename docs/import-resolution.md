# Import resolution

The initial import-resolution implementation expands imports into the existing
single-translation-unit compilation model.

## Syntax and lookup

Imports are top-level declarations and use a quoted relative path:

    import "math/geometry.vex"

Each path is resolved relative to the directory containing the file that declares
the import, not relative to the process working directory. Absolute paths are
rejected. String-literal escapes are decoded before filesystem lookup.

## Graph behavior

The loader canonicalizes paths and visits each imported file once. Repeated imports
are deduplicated, and cycles terminate without repeatedly loading files. Imported
files are placed before their importers in the combined source bundle. The source
bundle retains original file locations so diagnostics continue to name the file
that introduced an error.

Explicitly passing the same root file more than once remains an error-producing
duplicate declaration, preserving the previous multi-file command behavior.

## Current boundary

This is import expansion, not yet separate compilation or namespace support.
All imported declarations still share the translation unit's global namespace.
The check, ir, backend-emission, and native build/run compilation paths resolve
imports. The standalone parse command continues to parse only the file explicitly
named on its command line.

Future module work can add aliases, exported/private visibility, module interfaces,
and cached separate compilation without silently changing the current behavior.
