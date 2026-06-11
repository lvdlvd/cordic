# style.md — C preprocessor & declaration style

Guideline distilled from the cordic-math review. The unifying principle:
**prefer real C language constructs over preprocessor text substitution.**
The compiler can type-check, scope, and debug language constructs; it
cannot see inside macros. Reach for the preprocessor only when the
language genuinely cannot express the thing.

## Rules

1. **Integer constants: anonymous `enum`, not `#define`.**
   Applies to register field encodings, bit positions, fixed-point
   constants, table sizes, base addresses, tuning knobs. Enum constants
   are visible to the debugger, scoped by the compiler, and usable in
   `switch`. Group related constants in one enum block with a shared
   comment; named enum types (e.g. `enum cm_func`) where the set forms a
   logical type that can appear in signatures.

2. **Function-like macros: `static inline` functions.**
   Anything that takes arguments and computes a value — CSR composition,
   fixed-point conversion, bit manipulation helpers. Inline functions
   give argument type checking, single evaluation, and breakpoints, at
   zero cost at -O1 and above. This includes "constant-like" expressions
   that need a cast or a union trick (e.g. producing a canonical NaN, or
   turning a base address into a peripheral pointer: an enum for the
   address, an inline accessor for the pointer).

3. **Non-integer constants: `static const`, not `#define`.**
   Floats and doubles cannot be enum constants; `static const float` is
   the analogue. (In C they are not constant expressions, so this applies
   to values used in runtime code — which is almost all of them.)

4. **Include guards: `#pragma once`.**
   Universally supported by the toolchains we use (GCC, Clang, armclang),
   immune to guard-name collisions and copy-paste drift, three lines
   shorter.

5. **Single-consumer headers: merge into the consumer.**
   If a header is included by exactly one .c file, its content belongs at
   the head of that file. A header is an interface; one consumer means
   there is no interface, only indirection.

6. **No empty functions.**
   Do not keep no-op init/hook functions for symmetry or "future use".
   If a backend has no initialization, there is no init function; state
   the actual precondition (e.g. "application must enable the peripheral
   clock") in a comment at the point of relevance and in the public
   header. Dead hooks mislead readers into assuming they do something
   and must be called.

7. **Libraries do not touch shared system resources.**
   Clock enables, resets, global pin or bus configuration belong to the
   application, which owns the system view. The library documents its
   preconditions instead of acting on them.

## Legitimate uses of `#define` (the exceptions)

The preprocessor is the right tool when the language cannot do the job.
When an exception is used, a one-line comment at the definition saying
*why* is required if it is not obvious.

* **Standard-mandated macros**: `INFINITY`, `NAN`, `HUGE_VALF`, and the
  type-generic classifiers (`isnan`, `isinf`, ...) must be macros per
  the C standard; `M_PI` and friends stay `#ifndef`-guarded macros by
  ecosystem convention.
* **Values that do not fit `int`**: C11 enum constants must fit `int`,
  so masks like `0x80000000u` stay `#define` (or the whole related
  group does, for consistency).
* **Conditional compilation and build configuration**: anything tested
  with `#if`/`#ifdef` or meant to be overridden with `-D` (feature
  switches, retargetable I/O like a `PRINTF` hook). Note that a knob
  consumed only by `if (KNOB)` in code can still be an enum — the
  compiler folds it; only true preprocessor-level selection needs a
  macro.
* **Token manipulation**: symbol prefixing/renaming schemes, stringize/
  paste — textual by nature.
* **Qualifier shorthands matching an established register-header
  convention** (`__IO`, `__I`): consistency with the surrounding
  ecosystem beats purity.

## Rule of thumb

Before writing `#define X ...`, ask: would an `enum`, `static inline`,
or `static const` express this? If yes, use it. If no, write the macro
and say why in a comment.
