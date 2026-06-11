/*
 * math_emul.c — host backend: software model of the STM32G4 CORDIC.
 *
 * Models the documented engine: a 24-bit (q1.23) datapath driven for
 * PRECISION*4 iterations, in circular or hyperbolic mode, rotation or
 * vectoring. Pure integer arithmetic throughout; no floating point.
 *
 * The function-to-engine mapping is reconstructed from RM0440's range
 * limits, which it reproduces exactly:
 *   atan : vectoring of (2^-n, x*2^-n); max x = 128 since
 *          atan(128) = 1.563 < sum(atan 2^-i) = 1.743 rad
 *   ln   : vectoring of 2^-n*(x+1, x-1); z -> ln(x)/2, and the
 *          documented x <= 9.35 is ln(9.35)/2 = 1.118 = hyperbolic
 *          convergence bound
 *   sqrt : vectoring of (x*2^-n + c, x*2^-n - c), c = 2^-(n+2);
 *          modulus = sqrt(4c * x*2^-n) = 2^-n*sqrt(x), and the
 *          documented x >= 0.027 is 0.25*e^(-2*1.118) = 0.0267
 *
 * Per RM0440's precision notes, vectoring z is accumulated with the
 * angle-table entries pre-shifted right by SCALE (this is what makes the
 * "precision reduced proportionally" behavior fall out naturally, and
 * keeps the accumulator inside q1.23).
 *
 * Undocumented micro-architecture choices live in the calibration enum
 * below.
 * True silicon bit-exactness should be confirmed once with
 * test/device_dump (see README, "Calibration").
 */

#include <stdint.h>
#include "cordic_port.h"

/* ----------------------------------------------------------------------
 * Calibration knobs — every assumption about UNDOCUMENTED silicon
 * behavior is isolated here. RM0440 documents the CORDIC's I/O behavior;
 * ST training material (and AN5325) add that the engine datapath —
 * shifters, adders and angle table — is 24-bit (q1.23). The remaining
 * micro-architectural choices below are not published. After running
 * test/device_dump on real silicon and diffing against `make vectors`,
 * bit-exact calibration is a matter of flipping these values — not
 * rewriting the engine. Defaults are the simplest plausible hardware
 * choices.
 * -------------------------------------------------------------------- */
enum {
    /* q1.31 -> q1.23 narrowing of input arguments:
     * 0 = truncate (take top 24 bits, floor), 1 = round to nearest.
     * (q1.23 -> q1.31 widening is left-shift by 8, zero fill.) */
    CM_EMUL_NARROW_ROUND = 0,

    /* Phase (atan2) result when the accumulated angle nudges past +-1.0
     * (i.e. +-pi): RM0440 notes results "close to pi may sometimes wrap
     * to -pi", so the default models 24-bit two's-complement wrap.
     * 0 = saturate, 1 = wrap. */
    CM_EMUL_PHASE_WRAP = 1,

    /* Hyperbolic iteration repeat schedule (required for convergence).
     * Standard CORDIC repeats i = 4, 13, 40, ...; with <= 24 engine
     * steps only 4 and 13 are reachable.
     * Angle tables: entries are round-to-nearest of atan(2^-i)/pi and
     * atanh(2^-i) in q1.23; if silicon turns out to truncate,
     * regenerate them truncated. Gain correction is applied as a
     * post-multiply (vectoring) resp. seeds x0 (rotation). */
    CM_EMUL_HYP_REPEAT_A = 4,
    CM_EMUL_HYP_REPEAT_B = 13,
};


enum {
    ONE23  = 1 << 23,
    HALF23 = 1 << 22,
};

/* compile-time sanity: we rely on arithmetic right shift */
typedef char cm_assert_arith_shift[((-1) >> 1 == -1) ? 1 : -1];

/* ---- q1.23 tables (round-to-nearest; see tuning header) -------------- */

/* atan(2^-i)/pi, q1.23, i = 0..23 */
static const int32_t cm_atan_tab[24] = {
    2097152, 1238021, 654136, 332050, 166669, 83416, 41718, 20860,
    10430, 5215, 2608, 1304, 652, 326, 163, 81,
    41, 20, 10, 5, 3, 1, 1, 0,
};

/* atanh(2^-i), radians, q1.23, i = 1..24 ([0] unused) */
static const int32_t cm_atanh_tab[25] = {
    0,
    4607914, 2142558, 1054089, 524972, 262229, 131083, 65537, 32768,
    16384, 8192, 4096, 2048, 1024, 512, 256, 128,
    64, 32, 16, 8, 4, 2, 1, 1,
};

enum {
    INVK_Q23  = 5094007,  /* 1/K  = 0.6072529350088813 (circular gain),   q1.23 */
    INVKH_Q22 = 5064610,  /* 1/Kh = 1.2074970677630608 (hyperbolic gain), q2.22 */
};

/* ---- fixed-point primitives ------------------------------------------ */

static inline int32_t narrow_q31(int32_t q31)
{
    if (CM_EMUL_NARROW_ROUND)
        return (q31 + 0x80) >> 8;            /* round to nearest */
    return q31 >> 8;                         /* truncate: top 24 bits */
}

static inline int32_t widen_sat(int32_t q23)
{
    if (q23 >= ONE23)  return INT32_MAX;     /* saturate to  1 - 2^-31 */
    if (q23 < -ONE23)  return INT32_MIN;     /* saturate to -1         */
    return (int32_t)((uint32_t)q23 << 8);
}

static inline int32_t widen_wrap(int32_t q23)
{
    return (int32_t)((uint32_t)q23 << 8);    /* 24-bit two's-complement wrap */
}

static inline int32_t mul_q23(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * b) >> 23);
}

static inline int32_t mul_q22(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * b) >> 22);
}

/* ---- circular mode ---------------------------------------------------- */

/* Rotation: input angle/pi (q1.23, [-1,1)) and modulus m (q1.23).
 * Outputs m*cos and m*sin in q1.23. */
static void circ_rot(int32_t theta, int32_t m, int iters,
                     int32_t *xc, int32_t *ys)
{
    int32_t x = mul_q23(m, INVK_Q23);
    int32_t y = 0;
    int32_t z = theta;
    int i;

    /* quadrant pre-rotation: bring |z| <= 0.5 (converge bound 0.5549) */
    if (z > HALF23)       { int32_t t = x; x = -y; y = t;  z -= HALF23; }
    else if (z < -HALF23) { int32_t t = x; x = y;  y = -t; z += HALF23; }

    for (i = 0; i < iters && i < 24; i++) {
        int32_t xs = x >> i, yss = y >> i;
        if (z >= 0) { x -= yss; y += xs; z -= cm_atan_tab[i]; }
        else        { x += yss; y -= xs; z += cm_atan_tab[i]; }
    }
    *xc = x;
    *ys = y;
}

/* Vectoring: inputs (x, y) q1.23; outputs angle/pi (q1.23 value,
 * may transiently exceed +-1 by < 2^-22) and K*modulus. */
static void circ_vec(int32_t x, int32_t y, int scale, int iters,
                     int32_t *zout, int32_t *mout)
{
    int32_t z = 0;
    int i;

    /* pre-rotation into the right half-plane */
    if (x < 0) {
        int32_t t = x;
        if (y >= 0) { x = y;  y = -t; z = HALF23; }
        else        { x = -y; y = t;  z = -HALF23; }
    }

    for (i = 0; i < iters && i < 24; i++) {
        int32_t xs = x >> i, ys = y >> i;
        if (y >= 0) { x += ys; y -= xs; z += cm_atan_tab[i] >> scale; }
        else        { x -= ys; y += xs; z -= cm_atan_tab[i] >> scale; }
    }
    *zout = z;
    *mout = mul_q23(x, INVK_Q23);
}

/* ---- hyperbolic mode --------------------------------------------------- */

/* Iterate the hyperbolic recurrence for `steps` engine steps over the
 * index sequence 1,2,3,4,4,5,...,13,13,14,... (repeats per tuning hdr).
 * mode: 0 = rotation (drive z->0), 1 = vectoring (drive y->0).
 * Table entries are applied pre-shifted right by `scale`. */
static void hyp_engine(int32_t *px, int32_t *py, int32_t *pz,
                       int mode, int scale, int steps)
{
    int32_t x = *px, y = *py, z = *pz;
    int i = 1, rep_a = 0, rep_b = 0;
    int s;

    for (s = 0; s < steps && i < 25; s++) {
        int32_t xs = x >> i, ys = y >> i;
        int32_t alpha = cm_atanh_tab[i <= 24 ? i : 24] >> scale;
        int neg = mode ? (y >= 0) : (z < 0);

        if (!neg) { x += ys; y += xs; z -= alpha; }
        else      { x -= ys; y -= xs; z += alpha; }

        if (i == CM_EMUL_HYP_REPEAT_A && !rep_a)      rep_a = 1;
        else if (i == CM_EMUL_HYP_REPEAT_B && !rep_b) rep_b = 1;
        else i++;
    }
    *px = x; *py = y; *pz = z;
}

/* ---- backend entry ----------------------------------------------------- */

void cordic_backend_run(uint32_t csr, const int32_t args[2], int32_t res[2])
{
    unsigned func  = csr & 0xF;
    int iters      = (int)((csr >> 4) & 0xF) * 4;
    int scale      = (int)((csr >> 8) & 0x7);
    int32_t a1     = narrow_q31(args[0]);
    int32_t a2     = (csr & CM_CSR_NARGS) ? narrow_q31(args[1])
                                          : (INT32_MAX >> 8); /* reset ARG2 = +1 */
    int32_t x, y, z;

    res[0] = res[1] = 0;

    switch (func) {
    case CM_FUNC_COS:
    case CM_FUNC_SIN: {
        int32_t c, s;
        circ_rot(a1, a2, iters, &c, &s);
        if (func == CM_FUNC_COS) { res[0] = widen_sat(c); res[1] = widen_sat(s); }
        else                     { res[0] = widen_sat(s); res[1] = widen_sat(c); }
        break;
    }

    case CM_FUNC_PHASE:
    case CM_FUNC_MOD: {
        int32_t ph, m;
        circ_vec(a1, a2, 0, iters, &ph, &m);
        {
            int32_t phw = CM_EMUL_PHASE_WRAP ? widen_wrap(ph) : widen_sat(ph);
            int32_t mw  = widen_sat(m);      /* RM: saturates to 1 */
            if (func == CM_FUNC_PHASE) { res[0] = phw; res[1] = mw; }
            else                       { res[0] = mw;  res[1] = phw; }
        }
        break;
    }

    case CM_FUNC_ATAN:
        /* vector (2^-n, x*2^-n); z accumulates atan(x)/pi * 2^-n */
        x = ONE23 >> scale; y = a1; z = 0;
        {
            int i;
            for (i = 0; i < iters && i < 24; i++) {
                int32_t xs = x >> i, ys = y >> i;
                if (y >= 0) { x += ys; y -= xs; z += cm_atan_tab[i] >> scale; }
                else        { x -= ys; y += xs; z -= cm_atan_tab[i] >> scale; }
            }
        }
        res[0] = widen_sat(z);
        break;

    case CM_FUNC_COSH:
    case CM_FUNC_SINH:
        x = INVKH_Q22 >> (scale >= 1 ? scale - 1 : 0);  /* (1/Kh)*2^-n, q1.23 */
        y = 0; z = a1;
        hyp_engine(&x, &y, &z, 0, scale, iters);
        if (func == CM_FUNC_COSH) { res[0] = widen_sat(x); res[1] = widen_sat(y); }
        else                      { res[0] = widen_sat(y); res[1] = widen_sat(x); }
        break;

    case CM_FUNC_ATANH:
        x = ONE23 >> scale; y = a1; z = 0;
        hyp_engine(&x, &y, &z, 1, scale, iters);
        res[0] = widen_sat(z);
        break;

    case CM_FUNC_LN:
        /* vector 2^-n * (x+1, x-1); z -> 2^-n * ln(x)/2 = RES1 */
        x = a1 + (ONE23 >> scale);
        y = a1 - (ONE23 >> scale);
        z = 0;
        hyp_engine(&x, &y, &z, 1, scale, iters);
        res[0] = widen_sat(z);
        break;

    case CM_FUNC_SQRT:
        /* vector (a + c, a - c), c = 2^-(n+2); modulus -> 2^-n*sqrt(x) */
        x = a1 + (ONE23 >> (scale + 2));
        y = a1 - (ONE23 >> (scale + 2));
        z = 0;
        hyp_engine(&x, &y, &z, 1, scale, iters);
        res[0] = widen_sat(mul_q22(x, INVKH_Q22));
        break;

    default:
        break;
    }
}
