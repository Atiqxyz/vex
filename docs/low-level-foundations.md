# VEX low-level foundations

This is an architectural contract, not a distant feature wishlist. New language features must be designed around these constraints now, so later optimization strengthens the implementation without changing its fundamental model.

## Type identity and physical representation

Keep semantic type identity separate from physical representation. The compiler must explicitly model bit width, signedness, pointee type, mutability, nominal identity, array shape, size, alignment, field offsets, element stride, and ABI classification. A canonical recursive type descriptor should be shared by semantic analysis, IR verification, diagnostics, and backends. Target-specific layout belongs in a separate target data-layout service.

Backends must not infer language semantics from convenient host-language types. Target layout must determine pointer width, scalar size and alignment, aggregate size and alignment, field offsets, array stride, endianness, and ABI classification.

## Values, places, and memory

SSA values are not sufficient to model arbitrary addressable storage. Introduce first-class places before raw pointers, references, borrowing, volatile memory, or user-controlled allocation. A place describes addressable storage, its type, alignment, mutability, and provenance.

Memory operations must be explicit in IR: load, store, address calculation, field and element addressing, aggregate copy, allocation, reallocation, and deallocation. Alias-sensitive optimizations must preserve observable memory effects. Do not represent an addressable location as an ordinary SSA value merely because that is convenient for the current backend.

## Pointers, references, and slices

Raw pointers are typed (*T), target-sized, and do not imply ownership or bounds. Pointer arithmetic is scaled by the pointed-to type; byte arithmetic is explicit. Dereference, pointer arithmetic, and integer/pointer conversion require an explicit unsafe boundary.

References and slices are distinct abstractions. References carry lifetime and mutability constraints. Slices/views carry a pointer plus length; multidimensional views additionally carry shape and strides. Neither abstraction silently owns the referenced storage.

## Allocation and ownership

Allocation is an explicit policy. The allocator contract carries context, size, required alignment, and matching deallocation. Reallocation preserves allocator identity. Size arithmetic is checked before allocator calls.

Provide a useful default allocator while allowing custom arenas, pools, fixed buffers, and specialized allocators. Dynamic arrays and strings must retain enough allocator identity to be released by the allocator that created them. Do not introduce a tracing garbage collector as a hidden requirement.

Deterministic ownership and lifetime rules must eventually define transfer, borrowing, and cleanup on all structured control-flow exits. Existing program-exit cleanup is an interim implementation, not the final ownership model.

## Layout and ABI

The type/layout model must express native target layout, C-compatible layout, explicit alignment, and packed layouts with documented safety consequences. Struct offsets, aggregate sizes, pointer width, and calling conventions must be queryable for FFI and hardware-facing code.

Native struct layout is not portable serialization. Binary codecs remain explicit.

## Dependency order

1. Canonical recursive type descriptors and target-aware size/alignment/offset queries.
2. First-class places, load/store, address calculation, and verifier rules.
3. Typed raw pointers, address-of/dereference, pointer arithmetic, and explicit unsafe blocks.
4. Allocator values, aligned allocation, reallocation, deallocation, and ownership transfer.
5. References, slices, borrowing, lifetime checks, and invalidation rules.
6. Deterministic scope cleanup and destruction across all structured control-flow exits.
7. C ABI/FFI, representation attributes, volatile access, atomics, and memory ordering.

Higher-level features (enums, tagged unions, collections, modules, graphics resources, and concurrency) must use these foundations rather than inventing separate storage or lifetime rules.

## Compatibility constraints

- Preserve existing int (signed 64-bit), byte (unsigned 8-bit), and float (64-bit) behavior as the numeric family expands.
- Fixed arrays and fixed nested aggregates remain inline unless source code explicitly requests allocation.
- Dynamic collections remain reference-backed, with allocator identity and element layout made explicit.
- Never rely on C signed overflow or implementation-defined right shift of negative values.
- Every new IR operation must be typed, verified, source-spanned, and supported by both backends before being marked complete.
- The initial `TargetDataLayout` service computes primitive, string, fixed/dynamic nested-array, and structure layouts with checked size arithmetic. C emits size/alignment/field-offset assertions; LLVM uses the same expected alignment and validates target-computed nested aggregate sizes. The default data model is explicitly 64-bit, matching the current supported CI targets. Packed and C-ABI layout attributes remain future extensions.

