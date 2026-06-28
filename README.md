# cordic-math

`math.h` drop-in for cnav on the STM32G4, backed by the CORDIC
coprocessor, with a bit-exact host emulation of the engine for unit
testing on the build host.

```
include/math.h              drop-in header (the 12 cnav functions + sincosf)
src/cordic_math.c           shared frontend — float layer, backend-agnostic
src/cordic_port.h           backend ABI (CSR image + q1.31 args/results)
src/math_stm32.c            device backend: CORDIC registers, zero-overhead mode
src/math_emul.c             host backend: software model of the 24-bit engine
test/test_math.c            host conformance suite (vs. double-precision libm)
test/device_dump.c          deterministic vector dumper (host + device builds)
Makefile                    host test build; device flags documented inside
```

## Architecture: equality by construction

Everything above the register interface — float↔q1.31 conversion, range
reduction, special cases, result reconstruction — lives in
`cordic_math.c`, compiled unchanged for host and device. It uses only
integer/bit operations and IEEE-754 *single*-precision arithmetic, which
is bit-identical between the Cortex-M4F FPU and any IEEE host given the
build rules below. The backends speak raw hardware terms (`CSR` image,
q1.31 words), so the only place host and device can diverge is inside
the CORDIC engine itself — which the emulation models and the vector
harness verifies.

Function mapping (RM0440 §17.3.2): sinf/cosf/sincosf → SINE (one op
yields both); atan2f → PHASE, hypotf → MODULUS, both with exact common
power-of-two prescale into [0.25, 0.5); sqrtf → SQRT with x = m·4^k,
m ∈ [0.25, 1); logf → LN with x = m·2^k, m ∈ [0.5, 1), SCALE=1;
expf → COSH (e^r = cosh r + sinh r) with x = k·ln2 + r; powf =
expf(y·logf|x|) plus C99 special cases; log1pf = logf(1+x);
fmodf/fabsf/roundf are exact plain C (pure integer algorithms).

## Build rules (both sides, non-negotiable for bit-equality)

* `-ffp-contract=off` — no FMA contraction (M4F has VFMA, hosts have FMA3)
* no `-ffast-math` / `-funsafe-math-optimizations`
* `-fno-builtin` — the compiler must not substitute its own math knowledge
* device FPSCR left at reset: round-to-nearest, flush-to-zero **disabled**
* device: `-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard`,
  `-Iinclude` ahead of toolchain headers; sources `cordic_math.c` +
  `math_stm32.c`. The application must enable the CORDIC peripheral
  clock (RCC AHB1, CORDICEN) before the first math call — the library
  does no clock management.

NaNs are produced and propagated with bit operations only (canonical
0x7FC00000, or input payload with the quiet bit forced). Arithmetic NaN
generation is hardware-/optimizer-dependent and was measured to break
bit-equality; consequently the library does not raise FE_INVALID.
Verified on the host: output bit patterns are identical across
`-O0/-O2/-O3` over 300k random-bit-pattern argument sweeps, including
NaN/inf/subnormal inputs.

## Accuracy

The engine datapath is 24-bit q1.23 (ST training material, AN5325),
giving the RM0440 Table 115 error floors. Measured against
double-precision libm (host emulation, 2M-point sweeps):

| function | error type | measured max | budget asserted in tests |
|---|---|---|---|
| sinf, cosf  | absolute | 2.4e-6 (2^-18.7) | 4e-6 |
| atan2f      | absolute | 3.4e-6 | 8e-6 |
| hypotf      | relative | 4.3e-6 | 1.5e-5 |
| sqrtf       | relative | 2.3e-6 | 1.5e-5 |
| logf        | absolute | 4.6e-6 | 3e-5 |
| log1pf      | absolute | 4.9e-6 on (0, 1] | 3e-5 |
| expf        | relative | 6.3e-6 | 4e-5 |
| powf        | relative | grows ≈ (1+\|y·ln x\|)·2^-16 | same form |
| fmodf, fabsf, roundf | — | bit-exact vs. libm | bit-exact |

Notes:

* `sinf(x) = x` and `cosf(x) = 1` are returned exactly for |x| < 2^-12
  (true to < 2^-37 / 2^-25, far below the engine floor); `logf(1) = 0`
  exactly. These shortcuts live in the shared frontend, so bit-equality
  holds.
* Trig argument reduction uses a 3-term Cody–Waite π split (12-bit
  parts, products exact for |n| < 2^12): full accuracy to |x| ≈ 4096π,
  degrading gracefully beyond; |x| ≥ 2^24 is pre-folded by exact
  `fmodf` (single precision carries no meaningful phase there anyway).
* atan2f enforces sign(result) = sign(y), which repairs the documented
  near-π wrap of the PHASE function and the loss of y's sign when
  |y| ≪ |x| truncates to q1.31 zero.
* `log1pf` is `logf(1+x)` per the project spec. Its single call site
  (logAdd) feeds x = exp(d), d ≤ 0, i.e. x ∈ (0, 1]; measured absolute
  error there is < 5e-6 — verify against logAdd's tolerance in that
  session as planned.
* `powf` error grows with |y·ln x|; fine for moderate exponents.
* sqrtf alternative: the M4F `VSQRT.F32` is correctly rounded, faster
  than the CORDIC, and — because the host's `sqrtf` is also correctly
  rounded — would *also* be bit-exact host/device. The CORDIC path is
  used per the project spec; swap it if cycles matter.

## Testing

```
make test       # 3.09M checks vs. double libm: bounds, IEEE special
                # cases, bit-exactness of the exact functions
make vectors    # build/vectors_emul.txt — backend-level records
```

## Calibration against silicon (one-time)

The wrapper layers are equal by construction; the engine model is built
from documented behavior (q1.23 datapath, iteration counts, scaling
semantics) and reproduces every documented range limit exactly
(ln ≤ 9.35 ⇔ hyperbolic convergence bound 1.118; sqrt ≥ 0.027 ⇔
0.25·e^-2.236; atan ≤ 128 ⇔ Σatan 2^-i = 1.743). The remaining
micro-architecture choices (narrowing/table rounding, gain placement,
repeat schedule) are isolated in the calibration enum at the head of
`src/math_emul.c`.

To confirm or calibrate bit-exactness on real silicon:

1. Build `test/device_dump.c` + `src/math_stm32.c` for the target
   (retarget `printf` or define `CM_DUMP_PRINTF`), run, capture the
   text output. Records are `CSR ARG1 ARG2 RES1 RES2` in hex.
2. `make vectors` on the host.
3. `diff build/vectors_emul.txt device_capture.txt`.

A clean diff proves bit-exactness over every FUNC/SCALE operating point
the frontend uses (plus the full documented ranges and a precision
sweep). Any mismatch localizes to one function/scale/argument; flip the
corresponding knob and re-diff.

### Calibration status (against a real STM32G474, 2026-06)

A first silicon capture (3531-record sweep, generic G474 breakout) drove
the model from 3144 → **1492** differing records. **`cosh`, `sinh`,
`sqrt` are bit-exact**; the rest are within ~1–14 ULP of the bottom of a
24-bit result. Per function (differing/total):

| func | diff/total | | func | diff/total |
|------|-----------|-|------|-----------|
| cos  | 209/400   | | cosh | **0**/256 |
| sin  | 289/475   | | sinh | **0**/128 |
| phase| 355/464   | | atanh| 85/128    |
| mod  | 359/400   | | ln   | 172/512   |
| atan | 23/128    | | sqrt | **0**/640 |

What was fixed to get here (in capture order):

1. **Harness determinism (not the engine).** `rq31(INT32_MIN,
   INT32_MAX)` in `device_dump.c` combined `lo + (int32_t)off`, whose
   signed overflow resolved differently on arm-gcc vs the host, desyncing
   the *inputs* on every full-range sin/cos and atan draw (≈486 records).
   Fixed to an unsigned-modular combine — **both** sides must rebuild and
   the device must be re-captured.
2. **Gain reciprocals truncate, not round** (`INVK_Q23`, `INVKH_Q22` in
   the calibration enum): silicon truncates the gain-correction multiply.
   This alone made `sqrt` bit-exact.
3. **`NRES` contract.** `cordic_backend_run` was writing `res[1]`
   unconditionally; hardware writes RES2 only when the CSR's NRES bit is
   set. Single-result ops (phase/mod as the frontend issues them) now
   leave RES2 = 0, matching silicon.
4. **One guard bit in the angle path.** The angle accumulator `z` is
   q1.24 (`CM_EMUL_ANGLE_GUARD = 1`) while x/y stay q1.23. Found with the
   precision-sweep probe (below): for the angle-output functions the
   error grows with iteration count, and a single guard bit on `z` (and
   q1.24 angle tables) made `cosh`/`sinh` exact and roughly halved
   `atan`/`ln`. **G=0 and G≥2 are both worse**, and a *full* q24 datapath
   (guard bits on x/y too) is much worse — the guard bit is angle-only.
5. **Rotation decision is `z > 0`, not `z >= 0`** (z==0 takes the
   negative branch). −36 cos mismatches. Silicon derives the micro-
   rotation direction from a subtract/borrow, not a plain `z[MSB]` test.

### How the guard bit was localized — the precision-sweep probe

The CSR PRECISION field sets iteration count (4·P, capped at 24), so the
same input run at P=1..6 reads the silicon's result after 4,8,…,24
iterations. `examples/cordic` (in the sibling `n-array` repo) has a
`cm_dump_psweep()` that dumps sin+cos at P=1..6 for 64 PRNG-seeded
inputs, bracketed by `NARRAY-PSWEEP-DUMP … END`. Reproduce the inputs
host-side (same `xs32`/`rq31`) and find the first precision at which the
trajectories part: divergence at P=1 ⇒ range-reduction/seed; divergence
only at P=6 ⇒ tail-iteration accumulation. That bimodal split is what
pointed at the angle accumulator.

### Ruled out (tested against the capture, all worse or no help)

More than one angle guard bit (G≥2); guard bits on x/y (full q24);
toward-zero / round-half-up / round-half-even barrel shift (plain
arithmetic `floor` is correct); negate-before-shift in the add/sub;
rounding the guard bit at output; rounding the per-iteration
`table >> scale`; accumulate-angle-then-shift; rounding the seed/gain
multiply; vectoring (`y`-sign) and pre-rotation tie-breaks. The only
levers that helped were the five fixes above.

### Open threads (for the next pass)

- **cos/sin residual (~209/289).** Genuine decision flips — the `z>0`
  fix removed the z==0 ties but some remain, so there is a *second*
  decision subtlety (another comparator boundary, or the pre-rotation).
- **mod / ln / atanh: systematic ONE-SIGN bias** (silicon > emul by
  1–6 / 1–2 / small). Not random — these read off an accumulator that the
  emul leaves slightly low. But no global rounding/guard knob fixes it,
  and it's input-dependent (only some records, by +1/+2), i.e. it looks
  like a *specific internal truncation node*, not a clean output round.
- **phase:** a handful of catastrophic ±2²⁴ disagreements (wrap direction
  near ±π — RM0440 admits this is non-deterministic-looking) plus small
  rounding.
- **Unexplained circular/hyperbolic asymmetry** in the z guard-bit load
  (sin/cos real guard, cosh/sinh zero guard) — likely a SCALE/guard
  interaction worth modelling rather than special-casing.

### Is the RTL just buggy / non-ideal?

Plausibly, in part — and it changes the target. Signs that the residual
is implementation artifact rather than a clean math model we haven't
found: the **one-sided** bias on mod/ln/atanh (a *bias*, not symmetric
rounding error, is the fingerprint of a truncation that should have
rounded); the **z==0 → negative branch** off-by-one; and the
**circular/hyperbolic guard asymmetry** (a uniform design wouldn't have
one). But the decisive point is in **RM0440 Table 115, footnote (1)**:
the engine is specified *against double-precision float*, with **"an
additional rounding error ... of up to 2⁻²⁰ for q31 format"** and **no
specified rounding rule.** Our results are effectively q1.23 (LSB 2⁻²³),
so 2⁻²⁰ = 8 of the "bit-8 ULP" we measure — and our residual vs silicon
is mostly 1–6 of those. We were never chasing a bug; we were chasing the
explicitly-documented, unspecified 2⁻²⁰ rounding term. There is no rule
to match. (§17.3.4 likewise documents that scaling "entails a loss of
precision due to truncation" — the source of the scaled ln/atanh bias;
and Table 115 fn(2) notes phase/mod precision falls with the modulus.)
It is not noise, though: `cosh`/`sinh`/`sqrt` are
fully bit-exact and `atan` is 23/128, so the engine is deterministic and
largely modelable — the residual is a few concentrated quirks. Reaching
true bit-exactness may require modelling those quirks node-by-node (or
exhaustive per-(func,scale,iteration) characterization) rather than a
prettier closed form. Cheap confirmations worth running on the board:
(1) **determinism** — same input × N, identical bits? (2) **cross-part /
cross-family** — does another G4 (or G0/L5/H7) emit the same bits? If
revisions differ, it's silicon-rev-specific. (3) diff against **ST's own
CMSIS-DSP / X-CUBE CORDIC** reference model, if one exists.
- Re-capture after any `device_dump.c` change; keep the exact `.elf` used
  for a capture if you want to symbolize later.

### Determinism — confirmed

The CORDIC was re-run over the full 3531-vector set **1000×** on the
device (`cm_dump_determinism()` in `examples/cordic`): identical result
checksum every run, zero mismatches. So the residual is a **fixed,
reproducible datapath quirk**, not a race/metastability — and a captured
device vector set is a valid golden oracle.

### What this means for testing cnav

The host emulator is faithful to silicon **within RM0440's documented
error floor**, not bit-for-bit:

- **Bit-exact** today: `sqrtf`, `expf`/`coshf`-path (`cosh`/`sinh`), and
  anything built only on those.
- **Within ~1–14 ULP** (the bottom 1–2 bits of the 24-bit result):
  `sinf`/`cosf`/`sincosf` (`cos`/`sin`), `atan2f` (`phase`), `hypotf`
  (`mod`), `logf` (`ln`), `atanhf` (`atanh`). The bias on ln/mod/atanh is
  one-sided (silicon slightly high).

Practical guidance: **don't assert host==device bit-equality** for the
trig/log wrappers in cnav unit tests. Either (a) compare against
double-precision libm with the Table-115 tolerance (what `test_math.c`
already does), or (b) if you want exact golden-vector tests, capture the
**device** output as the golden reference (it's deterministic) rather
than the emulator. The emulator remains a faithful host stand-in for
behavior/accuracy testing; it is just not a bit-exact twin for the five
non-exact functions.

## Concurrency

The CORDIC is a global resource. Per cnav's runtime guarantee (single
thread, no floating point in IRQ handlers) `math_stm32.c` performs no
locking. If that guarantee ever changes, wrap `cordic_backend_run` in a
critical section.
