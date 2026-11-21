# Pointer-cell projection and addressability audit

**Audit date:** 2026-11-20  
**Audited `dev` baseline:** `d2fed76337d84a374a8b997d9d6bf1b94bd2a2b3`

## Finding

The next pointer-lifetime task is not merely to add more cases to the existing `**T` summary collector. The source language cannot yet represent most projected pointer cells.

The current implementation has useful foundations:

- `SemanticPointerType` records the pointee's scalar or nominal type, pointer depth, mutability, and (for known local/parameter addresses) a binding or parameter origin.
- Unary `&` creates a pointer only when its operand is a `NameExpr`. Member and index projections are rejected with `SEM070`.
- Typed IR places already represent structure-field and array-element projections. They support loads and stores, including projections rooted in a dereferenced structure pointer. A projected place is not automatically an addressable pointer value.
- Interprocedural pointer-cell summaries currently record pairs of one-based parameter indices: destination `**T` parameter and source `*T` parameter. They cover direct `*slot = source` stores, local aliases, named forwarding calls, and pointer-returning helpers that preserve a parameter origin.

But the aggregate type model does not carry pointer descriptors:

- `collect_struct_types()` calls `annotation_type()` for a field without a pointer-descriptor output, so a pointer-valued field is rejected.
- `SemanticArrayType` describes a scalar/nominal leaf and dimensions, but has no pointer leaf descriptor. Arrays of pointers are therefore not representable.
- Enum payload fields likewise have no pointer descriptor.
- The current call-site lifetime check treats a known destination as one whole local binding. It has no field/index projection identity.
- Dynamic-array element addresses would be unstable across `push` or `reserve` when growth reallocates the backing buffer.

Consequently, the phrase “projected pointer cells are conservatively rejected” should not be mistaken for a fully implemented projected-cell analysis: most such cells cannot currently be written in VEX source.

## Required semantic model

A pointer value and the location of a pointer cell are different facts. The next design should not overload `pointee_binding_id` with both.

Introduce a distinct addressable-location descriptor for pointers created from places. At minimum it needs:

1. **Storage root:** the binding or borrowed parameter whose storage contains the addressed cell.
2. **Projection path:** ordered structure-field indices and, where permitted, array-element indices.
3. **Pointee type:** exact scalar/nominal/pointer descriptor and indirection depth.
4. **Mutability:** whether the projected cell can be written through.
5. **Stability class:** fixed inline storage, function-local scalar/structure storage, or dynamic-buffer storage that may move.
6. **Lifetime origin:** the lifetime of the root storage, separate from the value currently stored in the pointer cell.

The descriptor must survive aliases and function arguments without silently changing the identity of the root cell. Reassigning the pointer value stored in a projected cell changes that cell's value provenance, not the location descriptor of an alias pointing to the cell.

## Dependency-aware implementation order

### 1. Pointer-capable aggregate types

Start with pointer-valued structure fields. Extend semantic field metadata, structure literals/member reads/member assignments, IR structure fields and values, verifier equality checks, and both backends. Pointer size/alignment must come from the shared target-layout service. Do not claim pointer arrays or pointer enum payloads until their descriptors are also carried end to end.

### 2. Stable projected address-of

Add address-of for supported stable projections such as a pointer-valued field in a mutable local structure and, after separate validation, fixed-array elements. Evaluate every index exactly once. The resulting pointer must retain both the root storage lifetime and the full projection path.

### 3. Field-level value provenance

Track the pointer value stored in each supported pointer field. Update it on direct field assignment and stores through aliases to that field. Join provenance across branches and loops; when different sources may reach a field, erase the exact origin. Structure copies must copy the field provenance alongside the value, while dynamic-array fields retain their existing shared-reference behavior.

### 4. Path-aware interprocedural summaries

Extend the current parameter-store summary from a pair of parameter indices to a destination parameter plus projection path and source parameter. At each call site, map the destination path to the actual root storage and compare every possible source lifetime against that root. Unknown or unsupported paths must remain conservative. Forwarding calls and pointer-returning helper chains must preserve the path, not just the source parameter index.

### 5. Array-element addresses only after invalidation rules

Fixed-array element addresses can be stable for the lifetime of their containing storage. Dynamic-array element addresses are different: a successful growth operation can move the backing buffer. Do not expose such addresses until VEX has a borrow/pinning/invalidation rule that prevents use after reallocation. Pointer arrays also need recursive pointer leaf metadata before their cells can be addressed.

## Control-flow origin joins

The whole-binding pointer-origin analysis snapshots pointer bindings before conditional branches and joins the resulting states. If every reachable branch preserves the same pointee binding/parameter origin, that exact origin remains available. If branches disagree, the analyzer deliberately erases the exact origin and marks it unknown; a later store through a `**T` parameter must then be rejected with `SEM072` unless a supported summary proves the source.

This is a conservative boundary, not a claim that every branch-dependent store is inherently unsafe. For example, two branches that both select the same pointer parameter are accepted, while a conditional choice between two different pointer parameters is currently rejected because the summary model stores one exact source parameter per store. Do not let the summary collector's AST traversal order choose whichever branch happened to be visited last.

Regression coverage in `tests/semantic_tests.cpp` now locks down both outcomes:
- a branch-dependent source with disagreeing origins is rejected;
- a branch join whose every arm agrees on one source parameter is accepted.

Supporting multiple possible source origins in one summary is a future analysis extension. It must represent the union of possible origins and prove every candidate at the call site; it must not weaken the current unknown-origin rejection.

## Required regression matrix

Before marking projected pointer cells implemented, add semantic and native C/LLVM tests for:

- address-of a pointer-valued structure field;
- direct write through the resulting `**T` pointer;
- aliases to the projected cell and repeated rebinding;
- safe source lifetimes and rejected shorter-lived sources;
- structure copies and nested structure-field paths;
- branch and loop joins where different pointer origins can reach the same field;
- forwarding helpers and helper-returned pointers;
- fixed-array pointer cells, if supported;
- dynamic-array element-address invalidation, which must be rejected until a borrow rule exists;
- exact pointee type, mutability, null, and unknown-provenance diagnostics.

The C and LLVM backends must agree on pointer field layout and the observed mutation. The verifier must reject malformed pointer descriptors and invalid projection paths independently of semantic analysis.

## Current status

This run does **not** implement pointer-valued aggregate fields or projected addresses. It corrects the diagnostic for pointer types used in unsupported declaration contexts, adds regression tests for the current rejection boundary, and records the prerequisite sequence so the next implementation does not build lifetime summaries on a location model that does not yet exist.
