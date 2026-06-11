/*
 * cordic_port.h — backend ABI between the shared frontend (cordic_math.c)
 * and the two interchangeable backends (math_stm32.c / math_emul.c).
 *
 * The frontend speaks to the backend in exactly the terms the hardware
 * understands: a CSR image plus q1.31 argument/result words. Everything
 * above this line is shared code, so any host/device divergence can only
 * originate inside the backend — which is the point of the design.
 */
#ifndef CORDIC_PORT_H
#define CORDIC_PORT_H

#include <stdint.h>

/* CSR field encodings (RM0440 §17.4.1) */
enum cm_func {
    CM_FUNC_COS   = 0,
    CM_FUNC_SIN   = 1,
    CM_FUNC_PHASE = 2,
    CM_FUNC_MOD   = 3,
    CM_FUNC_ATAN  = 4,
    CM_FUNC_COSH  = 5,
    CM_FUNC_SINH  = 6,
    CM_FUNC_ATANH = 7,
    CM_FUNC_LN    = 8,
    CM_FUNC_SQRT  = 9,
};

enum {
    CM_CSR_NRES  = 1 << 19,
    CM_CSR_NARGS = 1 << 20,
};

/* Compose a CSR image. precision = iterations/4 (1..15), scale = 0..7. */
#define CM_CSR(func, precision, scale, two_args, two_res)            \
    ((uint32_t)(func) | ((uint32_t)(precision) << 4) |               \
     ((uint32_t)(scale) << 8) |                                      \
     ((two_args) ? CM_CSR_NARGS : 0u) | ((two_res) ? CM_CSR_NRES : 0u))

/* PRECISION values used by the frontend — shared by construction.
 * Per RM0440 Table 115 these reach the engine's q1.23 quantization floor. */
enum {
    CM_PREC_TRIG = 6,    /* 24 iterations: sin/cos/phase/mod/atan, 2^-19 */
    CM_PREC_HYP  = 6,    /* 24 iterations: cosh/sinh/atanh/ln,     2^-18 */
    CM_PREC_SQRT = 3,    /* 12 iterations: sqrt,                   2^-19 */
};

/*
 * Run one CORDIC operation.
 *   csr    — full CSR image (FUNC, PRECISION, SCALE, NARGS, NRES)
 *   args   — q1.31 arguments; args[1] ignored unless NARGS is set
 *   res    — q1.31 results;   res[1] written only if NRES is set
 *
 * Provided by math_stm32.c (writes the registers, zero-overhead mode)
 * or by math_emul.c (software q1.23 engine).
 */
void cordic_backend_run(uint32_t csr, const int32_t args[2], int32_t res[2]);

#endif /* CORDIC_PORT_H */
