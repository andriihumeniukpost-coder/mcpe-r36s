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
