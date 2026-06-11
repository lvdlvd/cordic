/*
 * cordic_emul_tuning.h — calibration knobs for the software CORDIC model.
 *
 * RM0440 documents the CORDIC's I/O behavior; ST training material (and
 * AN5325) add that the engine datapath — shifters, adders and angle
 * table — is 24-bit (q1.23). The remaining micro-architectural choices
 * (rounding of the q1.31 -> q1.23 narrowing, rounding of the angle table
 * entries, gain-correction placement) are not published. Every such
 * assumption is isolated here so that, after running test/device_dump on
 * real silicon and diffing against the host (`make vectors`), bit-exact
 * calibration is a matter of flipping these knobs — not rewriting the
 * engine.
 *
 * Defaults are the simplest plausible hardware choices.
 */
#ifndef CORDIC_EMUL_TUNING_H
#define CORDIC_EMUL_TUNING_H

/* q1.31 -> q1.23 narrowing of input arguments.
 * 0 = truncate (take top 24 bits, floor)   [default]
 * 1 = round to nearest                                                 */
#define CM_EMUL_NARROW_ROUND    0

/* q1.23 -> q1.31 widening of results.
 * Results are left-shifted by 8 with zero fill (low 8 bits zero).      */

/* Angle tables: entries are round-to-nearest of atan(2^-i)/pi and
 * atanh(2^-i) in q1.23. If silicon turns out to truncate, regenerate
 * with tools/gen_tables.py --truncate.                                 */

/* Gain correction placement.
 * 0 = post-multiply (vectoring modulus corrected after iterating;
 *     rotation modes seed x0 with m/K resp. 2^-n/Kh)   [default]       */
#define CM_EMUL_GAIN_POST       0

/* Phase (atan2) result behavior when the accumulated angle nudges past
 * +-1.0 (i.e. +-pi): RM0440 notes results "close to pi may sometimes
 * wrap to -pi", so the default models 24-bit two's-complement wrap.
 * 0 = saturate, 1 = wrap   [default 1]                                 */
#define CM_EMUL_PHASE_WRAP      1

/* Hyperbolic iteration repeat schedule (required for convergence).
 * Standard CORDIC repeats i = 4, 13, 40, ...; with <= 24 engine steps
 * only 4 and 13 are reachable.                                         */
#define CM_EMUL_HYP_REPEAT_A    4
#define CM_EMUL_HYP_REPEAT_B    13

#endif /* CORDIC_EMUL_TUNING_H */
