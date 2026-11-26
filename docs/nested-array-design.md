# Nested arrays and collections of collections

**Status:** Dynamic, mixed, and all-fixed jagged arrays are implemented across parsing, semantic analysis, recursive IR descriptors, verifier, lowering, and both C and LLVM backends. Native runtime tests cover 2D/3D ragged collections, mixed fixed/dynamic dimensions, all-fixed nested values, nested assignment, side-effecting indices, collection operations, function parameters/returns, structure fields, arrays of structures, copied nested structures with array fields, recursive dynamic structure fields, and dimension-specific value-versus-reference behavior. Nested fixed inline storage is bounded by a target-native 256 KiB per-object limit and a 512 KiB per-function aggregate-storage budget; contiguous rectangular multidimensional arrays remain separate.

This note records the semantic contract and remaining implementation boundaries. Repeated dimensions compile and execute for dynamic, mixed, and all-fixed jagged shapes. All-fixed inline storage is bounded by target-aware backend checks, with a conservative semantic lower-bound check before IR generation.

## 1. Problem and intended use

One-dimensional arrays already support primitive and structure elements, indexing, mutation, iteration, function parameters and returns, capacity, reservation, and shared dynamic-array aliases. Real workloads also need collections whose elements are collections:

- a variable number of simulation bodies, each with a variable number of force contributions;
- a grid of rows with different lengths;
- batches of byte buffers or strings;
- adjacency lists and trees;
- arrays of fixed-size vectors or records.

The intended feature is recursively nested arrays, not a special two-dimensional matrix type. The same type rules should work for two levels or many levels without adding a separate compiler feature for each depth.

## 2. Type syntax and dimension order

Extend array annotations by allowing repeated bracket suffixes. Read dimensions from left to right, with the first bracket describing the outermost collection:

~~~vex
int[]       // dynamic collection of int
int[3]      // fixed collection of 3 int values
int[][]     // dynamic collection of dynamic int collections
int[3][4]   // fixed collection of 3 rows, each a fixed collection of 4 int values
int[][4]    // dynamic collection of fixed 4-element rows
int[3][]    // fixed collection of 3 dynamic rows
~~~

This ordering preserves the familiar outer-count-first reading: the first index selects the outer collection, and the next index selects an element within that result. Thus, for **int[3][4]**, the first index is in the range 0–2 and the second is in the range 0–3.

Each dimension is independently fixed or dynamic. A fixed dimension has a positive length, subject to the existing per-dimension limit. An empty bracket pair denotes a dynamic dimension. Invalid, zero, overflowing, or malformed fixed lengths remain compile-time diagnostics.

The parser should consume the complete repeated suffix rather than interpreting the first bracket and leaving later brackets as stray tokens.

## 3. Jagged collections, not an implicit matrix layout

Nested arrays are **jagged collections**. Every dynamic row has its own length and capacity. Rows may have different lengths:

~~~vex
make main() {
    let rows: int[][] = [[1, 2], [3, 4, 5], []]
    say(length(rows))       // 3
    say(length(rows[0]))    // 2
    say(length(rows[1]))    // 3
    say(length(rows[2]))    // 0
}
~~~

A nested array does not promise one contiguous rectangular allocation. The outer collection is contiguous in its elements; a dynamic inner collection has its own header and allocation. This is appropriate for adjacency lists, ragged data, and irregular workloads.

A later multidimensional numerical-array feature must be designed separately. It may provide one contiguous allocation with explicit shape, storage order, and strides. It must not silently reinterpret a jagged **T[][]** as a rectangular matrix or promise row-major/column-major layout.

## 4. Copying, aliasing, and mutation

Preserve VEX's existing rules at every dimension:

1. A fixed array is an aggregate value. Copying it copies its elements recursively.
2. A dynamic array is a shared reference to a runtime header. Copying it copies the reference, not the collection.
3. A fixed array containing dynamic arrays copies the outer fixed aggregate, but its dynamic elements remain shared references.
4. A dynamic array containing fixed arrays stores each fixed element inline. Reading or popping one returns a fixed value.
5. A dynamic array containing dynamic arrays stores shared references to the inner headers. Reading or popping one returns an alias to that inner collection.

For example:

~~~vex
make main() {
    let rows: int[][] = [[1, 2], [3, 4]]
    let alias = rows

    push(rows[0], 9)
    say(length(alias[0])) // 3: both names reach the same row header

    rows[1][0] = 30
    say(alias[1][0])      // 30

    push(rows, [7, 8])
    say(length(alias))    // 3: the outer header is shared too
}
~~~

The shared-header rule is stronger than copying a Go slice descriptor: VEX aliases refer to the same dynamic-array header, so length and capacity changes are visible through every alias. Keep this behavior; silently switching to copied length/capacity descriptors at a nested level would make the language inconsistent.

Mutation through a nested path remains rooted in a named mutable binding. A const aggregate binding remains immutable through all of its nested fields and indices, matching current structure/array rules. Collection-mutating built-ins must accept a dynamic array reached through a nested index, such as **push(rows[index], value)**, and resolve the mutable root consistently.

No user-visible references or pointers to individual elements are introduced. An indexed fixed-array result is a value; an indexed dynamic-array result is a copied shared header reference. Consequently, dynamic-array growth does not invalidate a VEX reference to an element—there is no such reference in this feature.

## 5. Literals and contextual typing

Keep the current one-dimensional literal rule and apply it recursively:

- A non-empty array literal infers a fixed dimension from its element count when no context overrides that dimension.
- A compatible declared or expected array type supplies the dimension kind and fixed length at that level.
- Context must propagate recursively into nested array literals. For example, the type **int[][]** makes both the outer literal and each non-empty row dynamic.
- Empty literals require a contextual dynamic-array type at the level where they occur. Therefore **let rows: int[][] = [[], [1, 2]]** is valid, while an untyped empty inner literal remains ambiguous.
- Fixed dimensions must match exactly. Dynamic inner dimensions may differ in length, which is what makes jagged collections useful.
- Existing int-to-float widening may occur at scalar leaves, but it must not erase or alter array dimension shapes.
- Recursive type compatibility must compare every dimension and the nominal identity of a structure base type.

An unannotated nested literal such as **[[1, 2], [3, 4]]** therefore infers a fixed outer collection of two fixed rows of two integers. Use an explicit dynamic type when later growth or ragged rows are required.

## 6. Structures and recursive type cycles

Allow array-valued structure fields to contain more than one dimension. Keep nominal structure identity and the existing rule that structure fields may refer only to permitted declarations.

A field creates a by-value recursion cycle only when every array layer between the field and the structure element is fixed. Any dynamic layer makes that path indirect because the containing structure stores a dynamic-array reference rather than embedding the recursively contained elements inline.

Examples:

- **Node { children: Node[] }** remains valid.
- **Node { children: Node[][] }** is valid: the outer field is pointer-backed, and its elements are pointers to inner dynamic arrays.
- **Node { children: Node[2][3] }** is rejected: every layer embeds the next layer and ultimately embeds Node by value.
- **Node { children: Node[2][] }** and **Node { children: Node[][2] }** are indirect through a dynamic layer and do not create an inline recursive layout.

The cycle check must inspect the full dimension descriptor; checking only the outermost dimension would reject safe recursive shapes or admit an invalid all-fixed cycle.

## 7. Built-ins and evaluation rules

Apply existing collection operations to the outermost dimension of the supplied expression:

| Operation | Nested-array meaning |
|---|---|
| **length(value)** | Element count of the selected outer dimension; fixed dimensions can be constants, dynamic dimensions read their header. |
| **capacity(value)** | Capacity of the selected dynamic dimension only. |
| **reserve(value, n)** | Reserve an absolute minimum capacity for that selected dynamic dimension only. |
| **push(value, element)** | Append an element matching the complete remaining nested type. |
| **pop(value)** | Remove and return an element with the complete remaining nested type. |

Thus **reserve(rows, n)** affects the outer rows collection, while **reserve(rows[i], n)** affects only row *i*. The existing one-dimensional **join(string[])** and **byte_slice(byte[], ...)** remain intentionally specialized; nested arrays do not silently broaden their signatures.

For indexed assignment and compound assignment, evaluate the root-to-leaf path once, evaluate each index expression exactly once, then perform the leaf operation and rebuild any fixed aggregate values back to the root. The same exactly-once rule applies when a nested indexed array is passed to a mutating built-in.

For-in should work recursively: iterating a collection of arrays binds each row with its full remaining type, so a nested loop can iterate the next dimension. Preserve the existing dynamic-length-at-loop-header behavior.

## 8. Memory representation and lifetime

Use the existing representation recursively:

- A fixed dimension is an inline value aggregate.
- A dynamic dimension is a pointer to a tracked dynamic-array header.
- Dynamic-array element storage remains contiguous and uses the full target-native allocation size of the element type.
- A dynamic array of fixed arrays stores those fixed aggregates inline in its data buffer.
- A dynamic array of dynamic arrays stores inner header pointers in its data buffer.
- A structure element uses its target ABI allocation size, including padding. A fixed-array element uses its full aggregate allocation size. A dynamic-array element is pointer-sized.

Structure values are copied recursively by value. Every fixed dimension embedded in a structure is copied inline, but every dynamic dimension remains a shared header reference. Consequently:

- Copying a structure with an `int[][]` field shares the outer header and the inner row headers.
- Copying one with an `int[2][]` copies its two fixed outer slots, while the dynamic row headers in those slots remain shared. Replacing one slot in the copy is local to that copy; mutating an existing row is shared.
- Copying one with an `int[][2]` field shares the dynamic outer header. Its fixed rows are inline values in the shared buffer, so replacing a row through one alias is visible through the others; reading a row produces a value copy.

These rules compose through nested structures, structure parameters and returns, and structures stored as elements of dynamic arrays. The C and LLVM backends must preserve the same per-dimension representation rather than applying an all-deep-copy or all-shared policy to the containing structure.

All dynamic headers continue to be registered in the current program-lifetime allocation list. Cleanup can free each registered header and data buffer once; it does not need recursive ownership traversal because dynamic references are shared and there are no element destructors today. Popping or replacing an inner-array reference does not free that inner header. It remains tracked until normal generated-entry cleanup.

This deliberately retains the existing limitation: allocations are not released when a lexical scope ends, when an element is popped, or when an alias becomes unreachable. Nested arrays can therefore retain more allocations until program exit. Do not add reference counting, recursive frees, or per-scope cleanup as an incidental part of this feature; those require a separate ownership/lifetime design. A runtime trap still terminates the process, leaving process reclamation to the operating system.

Allocation-size multiplication, capacity growth, and target-native element-size calculations must remain overflow-checked. Allocation failure traps consistently in both backends.

### Nested fixed aggregate size and stack budget

Nested fixed dimensions multiply inline object size. The existing 65,536-element limit applies per dimension and does not bound the total byte size of a multi-layer fixed aggregate. VEX now applies two explicit limits:

- A materialized nested fixed aggregate object may not exceed **256 KiB** according to the target ABI's allocation size.
- A function may not account for more than **512 KiB** of nested fixed aggregate storage across its parameters, return-value allowance, SSA aggregate slots, branch-copy slots, and backend-generated aggregate temporaries.

The semantic analyzer and IR verifier reject any consecutive fixed-dimension run whose element-count lower bound alone exceeds 256 KiB, assuming the smallest possible one-byte element. This prevents obviously oversized nested types from reaching backend layout calculations. It is only a lower-bound check: smaller element counts may still exceed the limit once the real element type and target padding are included.

The C backend emits `_Static_assert` checks using the target C compiler's `sizeof` for each materialized nested fixed type and a synthetic per-function aggregate frame. The LLVM backend derives each type's allocation size from LLVM's target data layout, sums sizes with checked unsigned addition, and branches to `llvm.trap` before stack slots are allocated if either budget is exceeded. Dynamic alloca counts are zero on the failing path, so a code generator cannot reserve the rejected aggregate frame before the guard.

The budget applies to inline nested fixed prefixes, including nested fixed arrays embedded through by-value structures. A dynamic outer dimension is a shared pointer and does not itself consume nested fixed stack storage; materialized fixed elements are checked when they become values or backend temporaries. Existing one-dimensional fixed-array behavior is unchanged by this nested-only budget.

This is deliberately not a complete stack model. Scalar locals, ordinary one-dimensional fixed arrays, call depth, runtime frames, and platform startup overhead are not bounded by these two limits. Fixed arrays remain inline values and are never silently moved to the heap; programs requiring larger nested fixed objects should use dynamic arrays. A future configurable stack policy can be considered alongside the general stack/heap model.

## 9. Type metadata required by the compiler

The semantic model and IR carry one canonical flattened recursive shape: a scalar or nominal structure leaf type plus an ordered, outermost-first vector of fixed or dynamic dimensions. Indexing removes exactly the first dimension, and structural equality compares the leaf identity and every remaining dimension. This representation travels through bindings, structure fields, function parameters and returns, block parameters, SSA values, array operations, and call verification.

The descriptor supports all dimension combinations. Materialized nested fixed values are checked against the target-aware object and per-function storage budgets described above.

- a scalar or nominal structure base type;
- an ordered sequence of array dimensions, outermost first;
- each dimension marked fixed with a positive length or dynamic with no fixed length.

A flattened dimension vector is sufficient; recursive heap-allocated type nodes are not required. Structural equality must compare the base type, nominal structure ID, and every dimension. Indexing removes exactly the first dimension. The descriptor must travel through bindings, structure fields, function parameters and returns, block parameters, SSA values, array operations, and call verification.

The IR verifier must reject malformed descriptors (zero fixed lengths, invalid structure IDs, impossible void/unknown bases, or inconsistent operation results) recursively. The C and LLVM backends must derive element types and sizes from the same descriptor rather than reconstructing a partial shape from separate one-level fields.

### C backend

A dynamic dimension maps to the tracked runtime header pointer. A fixed dimension maps to a generated aggregate type whose element type is derived recursively. Type emission must recursively collect every required fixed-array wrapper and emit dependencies in a valid order, including wrappers containing earlier-declared structures or dynamic-array pointers.

The runtime's existing element-size check and checked allocation helpers remain applicable. Use C compiler **sizeof** for the recursively generated element type; do not calculate structure padding manually.

### LLVM backend

A dynamic dimension maps to an opaque pointer to the runtime header. A fixed dimension maps to an LLVM array aggregate whose element type is derived recursively. Continue to use target data-layout-derived allocation size for dynamic element strides; this naturally includes nested aggregate padding. Do not hand-code byte sizes from source-level element counts.

## 10. Reference comparison and VEX's choice

| Reference | Useful property | Difference VEX should preserve or reject |
|---|---|---|
| C++ **std::vector** | Contiguous dynamic storage, independent inner vectors, explicit capacity reservation. | C++ vector copies its element values; VEX dynamic arrays intentionally copy a shared header reference. |
| Rust **Vec** | Contiguous growable storage with explicit ownership and well-defined collection operations. | Rust's ownership/clone model is not VEX's current shared-reference model; do not introduce implicit deep copies or claim Rust-like borrowing safety. |
| Go slices | A small descriptor can refer to shared backing storage; append and reallocation make aliasing details important. | A copied Go slice descriptor can diverge in length/capacity after append; VEX aliases share one header, so those metadata updates remain visible. |

Useful primary references:
- [C++ std::vector](https://en.cppreference.com/w/cpp/container/vector)
- [Rust std::vec](https://doc.rust-lang.org/std/vec/index.html)
- [Go slices: the mechanics of append](https://go.dev/blog/slices)

VEX adopts contiguous storage within each individual dynamic dimension, geometric growth, explicit reservation, and separate per-row lengths. It keeps its own shared-header alias semantics. It does not adopt C++ deep-copy container behavior, Rust ownership, or Go's copied slice-header behavior.

## 11. Required implementation and test sequence

Do not implement this as a parser-only feature. The implemented dynamic/mixed jagged-array vertical slice is:

1. [x] Canonical multi-dimension descriptors and recursive equality/validation helpers.
2. [x] Repeated type-annotation parsing and malformed-suffix diagnostics.
3. [x] Semantic analysis for dynamic and mixed fixed/dynamic annotations, contextual nested literals, empty nested literals, function signatures, structure fields, assignment, mutability, built-ins, and recursive structure-cycle checks.
4. [x] Complete shapes through lowering and function/block values.
5. [x] Recursive IR verifier checks for array construction, get/set, push/pop, calls, returns, and structure fields.
6. [x] Recursive C type naming/emission and wrapper dependency ordering.
7. [x] Recursive LLVM aggregate types and target-native element-size calculations.
8. [x] Matching C and LLVM runtime fixtures in the four-platform CI matrix.
9. [x] Target-aware 256 KiB per-object and 512 KiB per-function nested-fixed storage budgets, including C `sizeof` assertions and LLVM target-layout checks before stack allocation.

Remaining work: broaden adversarial allocation-failure tests and audit program-lifetime allocation retention. The general stack/heap model remains unfinished.

Minimum test coverage:

- two and three dynamic dimensions; fixed, dynamic, and mixed dimension combinations, including all-fixed nested arrays;
- rectangular and ragged nested literals, contextual empty inner rows, and invalid shape mismatches;
- nested arrays in structure fields, arrays of structures, copied nested structures, function parameters/returns, and recursive dynamic structure fields;
- fixed outer copy isolation and dynamic inner alias sharing, including `int[2][]` and `int[][2]` fields inside copied structures;
- dynamic outer alias sharing after push, pop, reserve, and growth;
- nested indexed reads/writes and compound assignment with side-effecting index functions evaluated exactly once;
- nested push/pop/reserve/capacity, nested for-in loops, bounds failures, allocation-size overflow, and allocation failure;
- verifier rejection of malformed nested descriptors and mismatched nested call/field/element shapes;
- semantic lower-bound rejection, target-native per-object rejection, and per-function aggregate-budget rejection in both backends;
- C/LLVM agreement for structures with padding and arrays whose element types are nested aggregates.

## 12. Explicit non-goals and dependencies

This feature does not implement borrowed slices, element references, pointer arithmetic, reference counting, per-scope cleanup, user allocators, maps, generic types, or true contiguous multidimensional arrays. Those remain separate design decisions.

The current program-lifetime allocation registry is sufficient to make shared nested dynamic references safe for the existing language subset, although it can retain unreachable allocations until entry return. A future ownership or allocator model must revisit this policy deliberately.

The recursive descriptor, dynamic/mixed jagged arrays, and bounded all-fixed nested aggregates are now in place. The cross-backend runtime-allocation audit is recorded in [memory-runtime-audit.md](memory-runtime-audit.md). The remaining memory-model work is not nested-array correctness: it is a deliberate allocator and lifetime design before introducing scope-based release, reference counting, or manual deallocation. Keep enums and modules deferred until those foundations are settled.
