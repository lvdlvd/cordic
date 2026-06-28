/*
 * math.h — single-precision drop-in for the STM32G4.
 *
 * Declares the single-precision functions the application links against.
 * The implementation is backend-selected at link time:
 *   math_stm32.c — STM32G4 CORDIC coprocessor (device)
 *   math_emul.c  — software model of the CORDIC engine, calibrated to
 *                  silicon (host)
 * plus the shared, backend-agnostic frontend cordic_math.c.
 *
 * Build rules for cross-host bit-equality (see README):
 *   -ffp-contract=off, no -ffast-math, default IEEE rounding,
 *   FPSCR flush-to-zero left disabled on the device.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ---- constants ------------------------------------------------------- */

#define HUGE_VALF   (__builtin_huge_valf())
#define INFINITY    (__builtin_inff())
#define NAN         (__builtin_nanf(""))

#ifndef M_PI
#define M_PI        3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2      1.57079632679489661923
#endif
#ifndef M_E
#define M_E         2.7182818284590452354
#endif

/* ---- classification (minimal, macro-based) --------------------------- */

#define isnan(x)    (__builtin_isnan(x))
#define isinf(x)    (__builtin_isinf(x))
#define isfinite(x) (__builtin_isfinite(x))
#define signbit(x)  (__builtin_signbit(x))

/* ---- optional symbol prefixing for host conformance tests ------------ */
/*
 * When CORDIC_MATH_PREFIX is defined (test builds only), every public
 * function is renamed cm_<name> so the host's real libm remains usable
 * as a reference in the same executable.
 */
#ifdef CORDIC_MATH_PREFIX
#define sinf     cm_sinf
#define cosf     cm_cosf
#define sincosf  cm_sincosf
#define atan2f   cm_atan2f
#define hypotf   cm_hypotf
#define sqrtf    cm_sqrtf
#define expf     cm_expf
#define logf     cm_logf
#define log1pf   cm_log1pf
#define powf     cm_powf
#define fmodf    cm_fmodf
#define fabsf    cm_fabsf
#define roundf   cm_roundf
#endif

/* ---- the provided single-precision functions -------------------------- */

float sinf(float x);
float cosf(float x);
float atan2f(float y, float x);
float hypotf(float x, float y);
float sqrtf(float x);
float expf(float x);
float logf(float x);
float log1pf(float x);
float powf(float x, float y);
float fmodf(float x, float y);
float fabsf(float x);
float roundf(float x);

/* Extension: the CORDIC computes sin and cos simultaneously; this returns
 * both for the price of one operation (GNU sincosf signature). */
void sincosf(float x, float *s, float *c);

/* NOTE: on the device, the application must enable the CORDIC peripheral
 * clock (RCC AHB1, CORDICEN) before using this library. There is no init
 * function. */

#ifdef __cplusplus
}
#endif
