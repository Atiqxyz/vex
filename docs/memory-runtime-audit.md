# Runtime allocation and cleanup audit

**Status:** The current compiler-managed allocation paths have been reviewed across the C and LLVM backends. This documents the existing runtime policy; it does not establish a general VEX ownership or allocator model.

## Allocation classes

| Allocation | C backend | LLVM backend | Lifetime tracking |
|---|---|---|---|
| Dynamic-array header | Zero-initialized `calloc(1, sizeof(vex_array))` with a null check | Allocation size is derived from the target layout of `%vex.array`, with a null check and explicit field initialization | Header is linked into the array registry only after successful allocation |
| Dynamic-array element buffer | `realloc` with checked element-size multiplication and a null check | `realloc` with checked unsigned multiplication and a null check | Owned by its registered header; successful reallocation updates the header |
| Concatenated/joined string | One allocation for tracking node plus byte payload | One allocation for tracking node plus byte payload | Separate string registry |
| Fixed array / fixed nested aggregate | Inline value aggregate | Inline LLVM aggregate / guarded stack slot | No heap registry entry |

A failed `realloc` leaves the old buffer pointer valid under the C allocation contract. Both generated runtimes terminate immediately on failure, so they do not continue with a partially updated header. Capacity and length are changed only after successful allocation and copying.

## Normal completion and traps

- The generated entry wrapper releases every registered dynamic-array header and its current element buffer exactly once.
- It then releases every tracked concatenation/join string block.
- C and LLVM perform the same cleanup before returning a valid process status and before trapping on an integer `main` result outside 0 through 255.
- Nested dynamic arrays do not require recursive destruction: each header is independently registered. Releasing an outer buffer does not dereference its elements; each inner header is later released from the same registry. This also handles shared aliases and recursive dynamic-array fields without double-free traversal.
- A runtime trap does not run VEX cleanup or unwind lexical scopes. The C runtime exits the process; the LLVM runtime emits a trap. The operating system reclaims process memory. Runtime trap wording and process-level termination details are not specified as a portable user-facing error API.

## Deliberate retention policy

Array headers and string blocks live until the generated entry returns. Popping an inner array, overwriting an element, leaving a block, or losing the last source-level alias does not release the allocation. This is currently deliberate: dynamic arrays are shared header references, but VEX has no reference counting, borrowing checker, ownership transfer, or destructor semantics. The policy avoids freeing storage still reachable through an alias, at the cost of retaining unreachable allocations in long-running programs.

Do not add recursive frees or scope-exit cleanup as a nested-array patch. Any earlier release requires a language-level lifetime model that accounts for copied structures, nested fixed arrays, dynamic aliases, recursive data, function arguments/returns, and control-flow exits.

## Failure coverage and limits

The runtime checks:
- header, buffer-growth, reservation, concatenation, and join allocation results;
- element-count multiplication and string-length / allocation-size addition overflow;
- negative reservation, capacity and length bounds, array index bounds, and empty pop.

Existing native integration tests exercise overflow, negative reservation, empty pop, bounds failures, nested aliasing, reserve/growth, and both backends. Backend tests also inspect the generated allocation-failure branches and cleanup operations.

**A deterministic out-of-memory integration test is not yet present.** The current runtime has no injectable allocator boundary. Adding an environment-variable fault switch or a production test-only global would create hidden runtime behavior; instead, allocator injection should be designed as part of a future explicit allocator/runtime boundary. Until then, allocation-failure branches are verified structurally, not by forcing the host allocator to fail.

## Remaining memory-model work

- user-visible allocator interface and allocator selection;
- scope/resource cleanup and deterministic destruction;
- ownership, borrowing, and explicit deallocation;
- a general stack/heap policy beyond nested fixed aggregate budgets;
- deterministic allocation-failure injection through a designed allocator interface;
- sanitizer-enabled CI and stress tests for long-running allocation retention.

This audit does not mark those broader items complete.
