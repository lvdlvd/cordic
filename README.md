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
the model from 3144 → **1528** differing records. **`cosh`, `sinh`,
`sqrt` are bit-exact**; the rest are within ~1–14 ULP of the bottom of a
24-bit result. Per function (differing/total):

| func | diff/total | | func | diff/total |
|------|-----------|-|------|-----------|
| cos  | 246/400   | | cosh | **0**/256 |
| sin  | 288/475   | | sinh | **0**/128 |
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

### Open threads (for the next pass)

- **cos/sin/phase/mod residual (~250–360 each).** The decisions are now
  right (guard bit), x/y are q1.23, yet the x/y *outputs* still drift.
  Next suspects: the seed `mul_q23(m, INVK)` rounding, or a guard bit
  that affects the x/y *carry* without widening their storage.
- **`atanh`/`ln` still 85/172.** Hyperbolic vectoring z-output; likely a
  second-order guard/scale effect (these run at SCALE≥1).
- **Unexplained asymmetry.** `cosh`/`sinh` only reach exact when `z` is
  loaded with a *zero* guard bit (`a1 << GUARD`), whereas sin/cos need
  the *real* guard bit (`z_from_angle`). Both run different SCALE (0 vs
  1); a SCALE/guard interaction in the angle load is the likely cause and
  is worth modelling explicitly rather than special-casing.
- Re-capture after any `device_dump.c` change; keep the exact `.elf` used
  for a capture if you want to symbolize later.

## Concurrency

The CORDIC is a global resource. Per cnav's runtime guarantee (single
thread, no floating point in IRQ handlers) `math_stm32.c` performs no
locking. If that guarantee ever changes, wrap `cordic_backend_run` in a
critical section.
