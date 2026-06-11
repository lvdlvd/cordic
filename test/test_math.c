/*
 * test_math.c — host conformance suite.
 *
 * The library is built with -DCORDIC_MATH_PREFIX, renaming its public
 * symbols to cm_*, so the host's real libm coexists in this executable
 * as the reference. References are computed in double precision.
 *
 * Two classes of checks:
 *   1. Exact functions (fabsf, roundf, fmodf): bit-for-bit equality
 *      with libm.
 *   2. CORDIC-backed functions: error bounds derived from RM0440
 *      Table 115 (q1.23 engine noise floor), plus IEEE special-case
 *      tables (NaN / +-0 / +-inf semantics).
 *
 * Everything is deterministic: a fixed-seed xorshift32 drives the random
 * sweeps, so a failure reproduces exactly.
 */

#include <math.h>      /* the SYSTEM math.h — reference */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* library under test (prefixed symbols) */
extern float cm_sinf(float);
extern float cm_cosf(float);
extern void  cm_sincosf(float, float *, float *);
extern float cm_atan2f(float, float);
extern float cm_hypotf(float, float);
extern float cm_sqrtf(float);
extern float cm_expf(float);
extern float cm_logf(float);
extern float cm_log1pf(float);
extern float cm_powf(float, float);
extern float cm_fmodf(float, float);
extern float cm_fabsf(float);
extern float cm_roundf(float);
extern void  cordic_math_init(void);

static int g_fail, g_checks;

typedef union { float f; uint32_t u; } fb;
static uint32_t bits(float f) { fb v; v.f = f; return v.u; }
static float    mkf(uint32_t u) { fb v; v.u = u; return v.f; }

/* deterministic PRNG */
static uint32_t rng_state = 1; /* seeded in main */
static uint32_t xs32(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}
/* uniform in [a, b] */
static float frand(float a, float b)
{
    return a + (b - a) * ((float)(xs32() >> 8) * 0x1p-24f);
}

/* ---- reporting -------------------------------------------------------- */

static void fail(const char *what, float in1, float in2,
                 float got, double want)
{
    g_fail++;
    if (g_fail <= 25)
        printf("FAIL %-8s in=(%.9g, %.9g) got=%.9g (0x%08x) want=%.17g\n",
               what, (double)in1, (double)in2, (double)got, bits(got), want);
}

static void chk_abs(const char *what, float in, float got, double want,
                    double bound)
{
    g_checks++;
    double e = (double)got - want;
    if (!(fabs(e) <= bound)) fail(what, in, 0, got, want);
}

static void chk_rel(const char *what, float in, float got, double want,
                    double bound)
{
    g_checks++;
    double e = ((double)got - want) / (want != 0.0 ? fabs(want) : 1.0);
    if (!(fabs(e) <= bound)) fail(what, in, 0, got, want);
}

static void chk_bits(const char *what, float in1, float in2,
                     float got, float want)
{
    g_checks++;
    /* NaNs compare by class, everything else bit-exact */
    if (isnan(want) && isnan(got)) return;
    if (bits(got) != bits(want)) fail(what, in1, in2, got, (double)want);
}

/* ---- error budgets (see README) --------------------------------------- */

#define B_SINCOS   4e-6    /* abs:  2^-19 engine + reduction slop          */
#define B_ATAN2    8e-6    /* abs:  2^-19 * pi                             */
#define B_HYPOT    1.5e-5  /* rel:  2^-19 on prescaled modulus >= 0.25     */
#define B_SQRT     1.5e-5  /* rel                                          */
#define B_LOG      3e-5    /* abs:  2^-18 * 4 (SCALE=1, x4 result scaling) */
#define B_EXP      4e-5    /* rel                                          */

/* ---- sweeps ------------------------------------------------------------ */

static void test_trig(void)
{
    int i;
    for (i = -40000; i <= 40000; i++) {           /* dense near origin */
        float x = (float)i * (4.0f * 3.14159265f / 40000.0f);
        chk_abs("sinf", x, cm_sinf(x), sin((double)x), B_SINCOS);
        chk_abs("cosf", x, cm_cosf(x), cos((double)x), B_SINCOS);
    }
    for (i = 0; i < 200000; i++) {                 /* wide random */
        float x = frand(-1000.0f, 1000.0f);
        chk_abs("sinf", x, cm_sinf(x), sin((double)x), B_SINCOS);
        chk_abs("cosf", x, cm_cosf(x), cos((double)x), B_SINCOS);
    }
    for (i = 0; i < 5000; i++) {                   /* tiny args: relative */
        float x = frand(-1.0f, 1.0f) * 0x1p-13f;
        chk_rel("sinf~0", x, cm_sinf(x), sin((double)x), 1e-6);
        chk_abs("cosf~0", x, cm_cosf(x), cos((double)x), 1e-6);
    }
    /* sincosf coherence with sinf/cosf */
    for (i = 0; i < 20000; i++) {
        float x = frand(-50.0f, 50.0f), s, c;
        cm_sincosf(x, &s, &c);
        chk_bits("sincos.s", x, 0, s, cm_sinf(x));
        chk_bits("sincos.c", x, 0, c, cm_cosf(x));
    }
}

static void test_atan2_hypot(void)
{
    int i;
    for (i = 0; i < 200000; i++) {
        float y = frand(-1.0f, 1.0f) * mkf(((xs32() % 80) + 87) << 23);
        float x = frand(-1.0f, 1.0f) * mkf(((xs32() % 80) + 87) << 23);
        if (x == 0.0f || y == 0.0f) continue;
        chk_abs("atan2f", y, cm_atan2f(y, x), atan2((double)y, (double)x),
                B_ATAN2);
        chk_rel("hypotf", y, cm_hypotf(x, y), hypot((double)x, (double)y),
                B_HYPOT);
    }
    /* extreme exponent separation */
    chk_rel("hypotf", 0, cm_hypotf(1e20f, 1e-20f), 1e20, B_HYPOT);
    chk_rel("hypotf", 0, cm_hypotf(2e38f, 1e38f), hypot(2e38, 1e38), B_HYPOT);
    chk_bits("hypotf-ovf", 3e38f, 3e38f, cm_hypotf(3e38f, 3e38f),
             (float)INFINITY);                 /* true result > FLT_MAX */
    chk_rel("hypotf", 0, cm_hypotf(1e-44f, 1e-44f), hypot(1e-44, 1e-44), 2e-2);
}

static void test_sqrt_log_exp(void)
{
    int i;
    for (i = 0; i < 200000; i++) {
        float x = frand(0.0f, 1.0f) * mkf(((xs32() % 160) + 47) << 23);
        if (x == 0.0f) continue;
        chk_rel("sqrtf", x, cm_sqrtf(x), sqrt((double)x), B_SQRT);
        chk_abs("logf",  x, cm_logf(x),  log((double)x),  B_LOG);
    }
    for (i = 0; i < 200000; i++) {
        float x = frand(-87.0f, 88.0f);
        chk_rel("expf", x, cm_expf(x), exp((double)x), B_EXP);
    }
    for (i = 0; i < 50000; i++) {                 /* log1p over logAdd range */
        float x = frand(0.0f, 1.0f);
        chk_abs("log1pf", x, cm_log1pf(x), log1p((double)x), B_LOG);
    }
    for (i = 0; i < 20000; i++) {                 /* log1p tiny */
        float x = frand(-1.0f, 1.0f) * 0x1p-26f;
        chk_abs("log1pf~0", x, cm_log1pf(x), log1p((double)x), 1e-9);
    }
    /* exactness pins */
    chk_bits("logf(1)", 1, 0, cm_logf(1.0f), 0.0f);
    chk_rel("sqrtf", 4, cm_sqrtf(4.0f), 2.0, B_SQRT);
    chk_rel("sqrtf", 2, cm_sqrtf(2.0f), sqrt(2.0), B_SQRT);
    chk_rel("sqrtf", 0, cm_sqrtf(0x1p-149f), sqrt((double)0x1p-149f), B_SQRT);
    chk_rel("sqrtf", 0, cm_sqrtf(0x1.fffffep127f), sqrt((double)0x1.fffffep127f), B_SQRT);
}

static void test_pow(void)
{
    int i;
    for (i = 0; i < 100000; i++) {
        float x = frand(0.01f, 100.0f);
        float y = frand(-8.0f, 8.0f);
        double w = pow((double)x, (double)y);
        if (w < 1e-30 || w > 1e30) continue;
        /* error grows ~ |y*ln x| * 2^-16 */
        double b = 3e-5 * (1.0 + fabs((double)y * log((double)x)));
        chk_rel("powf", x, cm_powf(x, y), w, b);
    }
    for (i = 0; i < 20000; i++) {                 /* negative base, int exp */
        float x = -frand(0.01f, 50.0f);
        int   n = (int)(xs32() % 13) - 6;
        double w = pow((double)x, (double)n);
        double b = 3e-5 * (1.0 + fabs(n * log(fabs((double)x))));
        chk_rel("powf-int", x, cm_powf(x, (float)n), w, b);
    }
}

static void test_exact_funcs(void)
{
    int i;
    static const float pins[] = {
        0.0f, -0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 1.5f, -1.5f, 2.5f, -2.5f,
        0x1p23f, -0x1p23f, 0x1p23f - 0.5f, 0x1.fffffep127f, 0x1p-149f,
        -0x1p-149f, 0x1p-126f, 3.5f, -3.5f, 8388609.0f,
    };
    for (i = 0; i < (int)(sizeof pins / sizeof pins[0]); i++) {
        float x = pins[i];
        chk_bits("fabsf",  x, 0, cm_fabsf(x),  fabsf(x));
        chk_bits("roundf", x, 0, cm_roundf(x), roundf(x));
    }
    for (i = 0; i < 400000; i++) {
        float x = mkf(xs32());
        if (isnan(x)) continue;
        chk_bits("fabsf",  x, 0, cm_fabsf(x),  fabsf(x));
        chk_bits("roundf", x, 0, cm_roundf(x), roundf(x));
    }
    for (i = 0; i < 400000; i++) {
        float x = mkf(xs32()), y = mkf(xs32());
        if (isnan(x) || isnan(y)) continue;
        chk_bits("fmodf", x, y, cm_fmodf(x, y), fmodf(x, y));
    }
    /* fmodf around exponent edges incl. subnormals */
    for (i = 0; i < 100000; i++) {
        float x = mkf((xs32() & 0x807FFFFFu) | (((xs32() % 24) + 0) << 23));
        float y = mkf((xs32() & 0x807FFFFFu) | (((xs32() % 24) + 0) << 23));
        if (isnan(x) || isnan(y)) continue;
        chk_bits("fmodf-sn", x, y, cm_fmodf(x, y), fmodf(x, y));
    }
}

static void test_specials(void)
{
    const float inf = (float)INFINITY, nan = (float)NAN;
    float s, c;

    /* sin/cos */
    chk_bits("sinf", 0, 0, cm_sinf(0.0f), 0.0f);
    chk_bits("sinf", -0.0f, 0, cm_sinf(-0.0f), -0.0f);
    chk_bits("cosf", 0, 0, cm_cosf(0.0f), 1.0f);
    g_checks += 3;
    if (!isnan(cm_sinf(inf)))  fail("sinf(inf)", inf, 0, cm_sinf(inf), 0);
    if (!isnan(cm_cosf(-inf))) fail("cosf(-inf)", -inf, 0, cm_cosf(-inf), 0);
    if (!isnan(cm_sinf(nan)))  fail("sinf(nan)", nan, 0, cm_sinf(nan), 0);
    cm_sincosf(-0.0f, &s, &c);
    chk_bits("sincos0", -0.0f, 0, s, -0.0f);
    chk_bits("sincos0", -0.0f, 0, c, 1.0f);

    /* atan2 special table (C99 F.10.1.4) */
    {
        static const float cases[][3] = {
            /* y,      x,      expected */
            { 0.0f,   1.0f,   0.0f },
            { -0.0f,  1.0f,  -0.0f },
            { 0.0f,  -1.0f,   3.14159265358979f },
            { -0.0f, -1.0f,  -3.14159265358979f },
            { 0.0f,   0.0f,   0.0f },
            { -0.0f,  0.0f,  -0.0f },
            { 0.0f,  -0.0f,   3.14159265358979f },
            { -0.0f, -0.0f,  -3.14159265358979f },
            { 1.0f,   0.0f,   1.57079632679490f },
            { -1.0f,  0.0f,  -1.57079632679490f },
            { 1.0f,   INFINITY,   0.0f },
            { -1.0f,  INFINITY,  -0.0f },
            { 1.0f,  -INFINITY,   3.14159265358979f },
            { -1.0f, -INFINITY,  -3.14159265358979f },
            { INFINITY,  1.0f,    1.57079632679490f },
            { -INFINITY, 1.0f,   -1.57079632679490f },
            { INFINITY,  INFINITY,  0.785398163397448f },
            { INFINITY, -INFINITY,  2.35619449019234f },
            { -INFINITY, INFINITY, -0.785398163397448f },
            { -INFINITY,-INFINITY, -2.35619449019234f },
        };
        int i;
        for (i = 0; i < (int)(sizeof cases / sizeof cases[0]); i++) {
            float got = cm_atan2f(cases[i][0], cases[i][1]);
            float want = (float)atan2((double)cases[i][0], (double)cases[i][1]);
            chk_bits("atan2sp", cases[i][0], cases[i][1], got, want);
        }
        if (!isnan(cm_atan2f(nan, 1.0f))) fail("atan2(nan,1)", nan, 1, 0, 0);
        if (!isnan(cm_atan2f(1.0f, nan))) fail("atan2(1,nan)", 1, nan, 0, 0);
    }

    /* hypot */
    chk_bits("hypot", inf, nan, cm_hypotf(inf, nan), inf);
    chk_bits("hypot", nan, -inf, cm_hypotf(nan, -inf), inf);
    if (!isnan(cm_hypotf(nan, 1.0f))) fail("hypot(nan,1)", nan, 1, 0, 0);
    chk_bits("hypot", 0, 0, cm_hypotf(0.0f, -0.0f), 0.0f);
    chk_bits("hypot", -3, 0, cm_hypotf(-3.0f, 0.0f), 3.0f);

    /* sqrt */
    chk_bits("sqrtf", 0, 0, cm_sqrtf(0.0f), 0.0f);
    chk_bits("sqrtf", -0.0f, 0, cm_sqrtf(-0.0f), -0.0f);
    chk_bits("sqrtf", inf, 0, cm_sqrtf(inf), inf);
    if (!isnan(cm_sqrtf(-1.0f))) fail("sqrt(-1)", -1, 0, cm_sqrtf(-1.0f), 0);
    if (!isnan(cm_sqrtf(nan)))   fail("sqrt(nan)", nan, 0, 0, 0);

    /* log / log1p */
    chk_bits("logf", 0, 0, cm_logf(0.0f), -inf);
    chk_bits("logf", -0.0f, 0, cm_logf(-0.0f), -inf);
    chk_bits("logf", inf, 0, cm_logf(inf), inf);
    if (!isnan(cm_logf(-1.0f))) fail("log(-1)", -1, 0, 0, 0);
    chk_bits("log1pf", -1, 0, cm_log1pf(-1.0f), -inf);
    if (!isnan(cm_log1pf(-2.0f))) fail("log1p(-2)", -2, 0, 0, 0);
    chk_bits("log1pf", 0, 0, cm_log1pf(0.0f), 0.0f);
    chk_bits("log1pf", -0.0f, 0, cm_log1pf(-0.0f), -0.0f);
    chk_bits("log1pf", inf, 0, cm_log1pf(inf), inf);

    /* exp */
    chk_bits("expf", inf, 0, cm_expf(inf), inf);
    chk_bits("expf", -inf, 0, cm_expf(-inf), 0.0f);
    chk_bits("expf", 200, 0, cm_expf(200.0f), inf);
    chk_bits("expf", -200, 0, cm_expf(-200.0f), 0.0f);
    if (!isnan(cm_expf(nan))) fail("exp(nan)", nan, 0, 0, 0);

    /* pow specials */
    chk_bits("powf", nan, 0, cm_powf(nan, 0.0f), 1.0f);
    chk_bits("powf", 1, nan, cm_powf(1.0f, nan), 1.0f);
    chk_bits("powf", 0, 3, cm_powf(0.0f, 3.0f), 0.0f);
    chk_bits("powf", -0.0f, 3, cm_powf(-0.0f, 3.0f), -0.0f);
    chk_bits("powf", -0.0f, 4, cm_powf(-0.0f, 4.0f), 0.0f);
    chk_bits("powf", 0, -3, cm_powf(0.0f, -3.0f), inf);
    chk_bits("powf", -0.0f, -3, cm_powf(-0.0f, -3.0f), -inf);
    chk_bits("powf", 2, inf, cm_powf(2.0f, inf), inf);
    chk_bits("powf", 2, -inf, cm_powf(2.0f, -inf), 0.0f);
    chk_bits("powf", 0.5f, inf, cm_powf(0.5f, inf), 0.0f);
    chk_bits("powf", -1, inf, cm_powf(-1.0f, inf), 1.0f);
    chk_bits("powf", -inf, 3, cm_powf(-inf, 3.0f), -inf);
    chk_bits("powf", -inf, 4, cm_powf(-inf, 4.0f), inf);
    chk_bits("powf", -inf, -3, cm_powf(-inf, -3.0f), -0.0f);
    chk_bits("powf", inf, -1, cm_powf(inf, -1.0f), 0.0f);
    if (!isnan(cm_powf(-2.0f, 0.5f))) fail("pow(-2,.5)", -2, 0.5f, 0, 0);

    /* fmod specials */
    if (!isnan(cm_fmodf(1.0f, 0.0f))) fail("fmod(1,0)", 1, 0, 0, 0);
    if (!isnan(cm_fmodf(inf, 2.0f))) fail("fmod(inf,2)", inf, 2, 0, 0);
    chk_bits("fmodf", 5, inf, cm_fmodf(5.0f, inf), 5.0f);
    chk_bits("fmodf", -0.0f, 3, cm_fmodf(-0.0f, 3.0f), -0.0f);
}

int main(void)
{
    rng_state = 0xC0FFEE42u;
    cordic_math_init();

    test_exact_funcs();
    test_trig();
    test_atan2_hypot();
    test_sqrt_log_exp();
    test_pow();
    test_specials();

    printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
