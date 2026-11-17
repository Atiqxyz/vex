# VEX enums and tagged unions

**Status:** Design selected; implementation not yet started.

This design treats an enum as a nominal, inline tagged union. It is not a boxed object, an untagged union, or merely a collection of integer constants. The value contains a discriminant and storage for the largest variant payload. Construction and pattern matching are typed operations, and storage is calculated by the shared target data-layout service.

## 1. Problem and goals

Enums model a value that is exactly one of a closed set of alternatives. Payload-carrying variants let those alternatives contain different typed data without separate allocations.

Representative use cases include:

- A numerical solver result such as Converged(iterations, residual) or Diverged(reason).
- A simulation event such as Burn(start_time, delta_v), StageSeparated(stage), or TelemetryLost.
- Parser tokens or compiler diagnostics with distinct payloads.
- Explicit state machines where every valid state and its associated data are represented together.

The design must preserve nominal type identity, value semantics, deterministic layout, and exhaustive handling. Constructing or passing an enum must not allocate memory implicitly.

## 2. Syntax

An enum declaration lists variants in source order. A variant may carry zero or more positional fields:

~~~vex
enum SolverResult {
    Converged(int, float),
    Diverged(string),
    Cancelled
}
~~~

Construct a variant through its qualified name:

~~~vex
let result = SolverResult.Converged(42, 0.000001)
let stopped = SolverResult.Cancelled
~~~

Use one statement-form selection construct for exhaustive pattern handling:

~~~vex
match result {
    SolverResult.Converged(iterations, residual) => {
        say(iterations)
        say(residual)
    }
    SolverResult.Diverged(reason) => {
        say(reason)
    }
    SolverResult.Cancelled => {
        say("cancelled")
    }
}
~~~

A wildcard arm is written _ => { ... } and, when present, must be the final arm. Match arms contain blocks, do not fall through, and do not yield a value in the initial implementation. Expression-valued matching can be considered after control-flow and type-join semantics are established.

Payload fields are positional in this first design. Named payload fields, guards, nested destructuring, generic enums, explicit discriminant values, and user-selected tag widths are deferred rather than given partially specified behavior.

## 3. Type and semantic rules

1. Each enum declaration creates a nominal type with a unique compiler-assigned identity. Two declarations with identical variants are still different types.
2. Variant names are unique within their enum. Variant tags are stable zero-based uint32 values assigned in declaration order.
3. A constructor names the enum and variant explicitly. Its argument count and types must exactly match the variant payload, subject only to existing permitted conversions.
4. Every payload field is a storable VEX value type. void is not permitted. Existing fixed-array, dynamic-array, string, primitive, and structure types may be used.
5. A variant payload is stored inline. A fixed array or structure payload is copied by value. Existing dynamic-array and string fields retain their current reference-backed descriptor behavior; the enum does not acquire ownership that its payload fields do not already have.
6. By-value recursive layouts are rejected. A recursive path that crosses an existing dynamic-array reference is representable because the dynamic outer dimension is pointer-sized.
7. A match scrutinee is evaluated exactly once. Each arm creates a lexical scope. Payload bindings are immutable and visible only inside that arm.
8. A match is exhaustive when every declared variant appears exactly once or a final wildcard arm is present. Duplicate variants, unknown variants, wrong binding counts, duplicate binding names, and non-exhaustive matches are errors.
9. There is no implicit conversion between an enum and an integer. Reading a discriminant for low-level code requires an explicit built-in or representation operation introduced with the typed IR support.
10. Pattern arms are ordinary control-flow branches. return, break, and continue keep their existing meanings and target rules.

## 4. Physical representation

The initial representation is intentionally fixed and target-queryable:

- The discriminant is an unsigned 32-bit integer.
- Each variant payload is laid out like a tuple of its declared fields using the target's ordinary field-alignment rules.
- The payload area is a union: its size is the maximum variant payload size and its alignment is the maximum variant payload alignment.
- The payload offset is align_up(sizeof(uint32), payload_alignment).
- The complete enum alignment is the maximum of discriminant and payload alignment. The complete size is the payload offset plus union size, rounded up to the enum alignment.
- An enum whose variants have no payload is exactly a 4-byte, 4-aligned value under the default target layout.

The tag and payload are part of one inline value. Fixed arrays of enums and structures containing enums therefore use the same layout and stride rules as other aggregates. No variant is separately boxed, and no constructor performs a hidden allocation.

The first ABI is VEX-native, not automatically C-compatible. C-compatible enum layout, explicit representation attributes, packed layout, and foreign ABI guarantees require a separate design and must not be implied by matching the C spelling.

## 5. IR and backend model

The IR type table will gain a nominal enum descriptor containing its declaration identity and ordered variants, including each variant's payload field descriptors. The independent verifier will validate enum IDs, variant indices, payload arity/types, and match-edge payload types.

Construction, discriminant dispatch, and payload projection must be dedicated typed IR operations or terminators. A payload projection is valid only for the corresponding variant path; it must not become an unchecked general-purpose byte reinterpretation operation.

The C backend will emit a named structure containing the fixed-width tag and a union of named variant-payload structures. The LLVM backend will emit a target-layout-compatible aggregate, use the tag for an LLVM switch, and access payload fields through offsets calculated by the shared target layout model. Both backends must agree on tag values, payload offsets, enum size/alignment, and function/array value semantics.

The shared TargetDataLayout service must compute each variant's tuple layout, the maximum union size/alignment, payload offset, total enum size/alignment, and enum array stride. It must reject overflow and unsupported layouts rather than wrapping sizes. Nominal enum and variant declarations now live in the canonical IR module type table, with verifier validation for IDs, names, variant uniqueness, and payload descriptors. The layout service calculates discriminant and payload sizes, alignments, offsets, and declaration-order tags. These are declaration and layout foundations only; enum values, source syntax, construction, and matching remain unimplemented.

## 6. Alternatives considered

**C-style integer enums plus a separate payload union:** this keeps simple enums compact, but creates two type systems and two construction/matching paths. It is deferred in favor of one consistent sum-type model.

**Heap-boxed variant objects:** this simplifies moving differently sized payloads, but introduces hidden allocation, pointer indirection, and lifetime/allocator policy into ordinary value construction. It conflicts with VEX's explicit-storage direction and is rejected as the default representation.

**Untagged unions:** these have no discriminant and cannot support safe exhaustive matching by themselves. They are a separate low-level feature with explicit unsafe access rules, not a substitute for enums.

## 7. Implementation order and acceptance tests

1. Extend target type descriptors and TargetDataLayout with variant payload and enum aggregate calculations.
2. Add enum declaration AST and parsing, including malformed and duplicate-variant diagnostics.
3. Register nominal enum types and validate payload field types and recursive by-value layouts.
4. Add qualified variant construction and typed IR representation.
5. Add match statement parsing, scoped payload bindings, exhaustiveness checks, and typed control-flow lowering.
6. Add independent verifier rules and C/LLVM value construction, switching, and payload access.
7. Test unit variants, mixed payload sizes and alignments, nested structures/fixed arrays, dynamic references inside payloads, function parameters/returns, arrays of enums, exact-once scrutinee evaluation, and all invalid-match cases.
8. Assert matching size, alignment, discriminant and payload offsets in both backends on all four CI platforms.

The feature is complete only when construction, passing/returning, matching, layout, verifier checks, tests, and documentation work end to end in both backends.
