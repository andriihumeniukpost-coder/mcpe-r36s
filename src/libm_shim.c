/* "libm.so" shim: the game calls libm with the soft-float convention
 * (args/results in core registers); glibc on the R36S is hard-float.
 * Each wrapper below is exported with the base AAPCS and forwards to glibc. */
#define _GNU_SOURCE
#include "android_min.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

static void *mh;
static void *M(const char *n) {
    if (!mh) mh = dlopen("libm.so.6", RTLD_NOW);
    void *p = dlsym(mh, n);
    if (!p) { fprintf(stderr, "[libm] missing %s\n", n); abort(); }
    return p;
}

#define F_F(n)   EXPORT SOFT float n(float x)                    { static float (*r)(float); if (!r) r = M(#n); return r(x); }
#define F_FF(n)  EXPORT SOFT float n(float x, float y)           { static float (*r)(float, float); if (!r) r = M(#n); return r(x, y); }
#define D_D(n)   EXPORT SOFT double n(double x)                  { static double (*r)(double); if (!r) r = M(#n); return r(x); }
#define D_DD(n)  EXPORT SOFT double n(double x, double y)        { static double (*r)(double, double); if (!r) r = M(#n); return r(x, y); }

F_F(acosf) F_F(asinf) F_F(atanf) F_F(cosf) F_F(sinf) F_F(tanf) F_F(ceilf) F_F(floorf)
F_F(roundf) F_F(truncf) F_F(nearbyintf) F_F(exp2f) F_F(logf) F_F(sqrtf) F_F(fabsf) F_F(expf)
F_F(asinhf) F_F(log2f) F_F(log10f) F_F(coshf) F_F(sinhf) F_F(tanhf)
F_FF(atan2f) F_FF(powf) F_FF(fmodf) F_FF(fmaxf) F_FF(fminf)
D_D(acos) D_D(asin) D_D(atan) D_D(cos) D_D(sin) D_D(tan) D_D(ceil) D_D(floor) D_D(sqrt) D_D(exp) D_D(log) D_D(fabs)
D_DD(atan2) D_DD(pow) D_DD(fmod)
EXPORT SOFT double ldexp(double x, int e) { static double (*r)(double, int); if (!r) r = M("ldexp"); return r(x, e); }
EXPORT SOFT float ldexpf(float x, int e) { static float (*r)(float, int); if (!r) r = M("ldexpf"); return r(x, e); }

/* ---- the rest of what libminecraftpe / libfmod / libc++_shared import (1.19.x): without a wrapper these fell through to
 * glibc's hard-float versions and returned garbage or crashed (sincosf did: SIGBUS in libm). */
D_D(acosh) D_D(asinh) D_D(atanh) D_D(cbrt) D_D(cosh) D_D(exp2) D_D(expm1) D_D(log10) D_D(log1p) D_D(log2)
D_D(nearbyint) D_D(round) D_D(sinh) D_D(tanh) D_D(trunc)
F_F(cbrtf) F_F(rintf)
F_FF(hypotf)
D_DD(hypot) D_DD(remainder)
EXPORT SOFT long lrint(double x)   { static long (*r)(double); if (!r) r = M("lrint"); return r(x); }
EXPORT SOFT long lround(double x)  { static long (*r)(double); if (!r) r = M("lround"); return r(x); }
EXPORT SOFT long lroundf(float x)  { static long (*r)(float); if (!r) r = M("lroundf"); return r(x); }
EXPORT SOFT double frexp(double x, int *e)  { static double (*r)(double, int *); if (!r) r = M("frexp"); return r(x, e); }
EXPORT SOFT float frexpf(float x, int *e)   { static float (*r)(float, int *); if (!r) r = M("frexpf"); return r(x, e); }
EXPORT SOFT double modf(double x, double *i) { static double (*r)(double, double *); if (!r) r = M("modf"); return r(x, i); }
EXPORT SOFT float modff(float x, float *i)   { static float (*r)(float, float *); if (!r) r = M("modff"); return r(x, i); }
EXPORT SOFT double scalbn(double x, int e)   { static double (*r)(double, int); if (!r) r = M("scalbn"); return r(x, e); }
EXPORT SOFT void sincosf(float x, float *s, float *c) { static void (*r)(float, float *, float *); if (!r) r = M("sincosf"); r(x, s, c); }
EXPORT SOFT int isnan(double x) { return x != x; }
