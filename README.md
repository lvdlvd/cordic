# cordic-math

`math.h` drop-in on the STM32G4, backed by the CORDIC
coprocessor, with a bit-exact host emulation of the engine for unit
testing on the build host.

```
include/math.h              drop-in header 
src/cordic_math.c           shared frontend — float layer, backend-agnostic
src/cordic_port.h           backend ABI (CSR image + q1.31 args/results)
src/math_stm32.c            device backend: CORDIC registers, zero-overhead mode
src/math_emul.c             host backend: software model of the 24-bit engine
src/cordic_emul_tuning.h    knobs isolating undocumented silicon choices
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
repeat schedule) are isolated in `src/cordic_emul_tuning.h`.

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

## Concurrency

The CORDIC is a global resource. Runtime shoul guarantee single
thread, no floating point in IRQ handlers `math_stm32.c` performs no
locking. If that guarantee ever changes, wrap `cordic_backend_run` in a
critical section.
