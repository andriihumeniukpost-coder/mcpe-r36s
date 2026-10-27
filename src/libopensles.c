/* libOpenSLES.so replacement for the original (Android) libfmod.so.
 * FMOD dlopen()s libOpenSLES.so and uses only: engine, output mix, one audio
 * player fed by an Android simple buffer queue, the play interface, and the
 * Android configuration interface. We back that with an SDL2 audio device.
 * (Sounds are .fsb banks, decoded by FMOD itself, so we only need PCM output.) */
#define _GNU_SOURCE
#include "android_min.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

typedef uint32_t SLuint32; typedef int32_t SLint32; typedef uint32_t SLresult; typedef uint32_t SLboolean;
typedef struct { uint32_t a; uint16_t b, c, d; uint8_t e[6]; } SLInterfaceID_;
typedef const SLInterfaceID_ *SLInterfaceID;
#define OK 0u
#define E_PARAM 2u
#define E_UNSUPPORTED 12u

#define IID(name, n) static const SLInterfaceID_ iid_##name = { 0x7e000000u + (n), 0, 0, 0, {0} }; \
                     EXPORT const SLInterfaceID SL_IID_##name = &iid_##name;
IID(ENGINE, 1) IID(PLAY, 2) IID(ANDROIDSIMPLEBUFFERQUEUE, 3) IID(ANDROIDCONFIGURATION, 4) IID(RECORD, 5) IID(VOLUME, 6)
/* plain OpenSL ES buffer queue (used by Minecraft PE 0.8.x): same object as the Android simple queue */
EXPORT const SLInterfaceID SL_IID_BUFFERQUEUE = &iid_ANDROIDSIMPLEBUFFERQUEUE;
#include <math.h>

/* ------------------------------------------------------ object model */
enum { K_ENGINE, K_MIX, K_PLAYER };
typedef struct Obj Obj;

/* vtable layouts (OpenSLES.h) */
typedef struct SLObjectItf_ {
    SLresult (*Realize)(void *, SLboolean);
    SLresult (*Resume)(void *, SLboolean);
    SLresult (*GetState)(void *, SLuint32 *);
    SLresult (*GetInterface)(void *, SLInterfaceID, void *);
    SLresult (*RegisterCallback)(void *, void *, void *);
    void (*AbortAsyncOperation)(void *);
    void (*Destroy)(void *);
    SLresult (*SetPriority)(void *, SLint32, SLboolean);
    SLresult (*GetPriority)(void *, SLint32 *, SLboolean *);
    SLresult (*SetLossOfControlInterfaces)(void *, int16_t, void *, SLboolean);
} SLObjectItf_;

typedef struct SLEngineItf_ {
    void *CreateLEDDevice, *CreateVibraDevice;
    SLresult (*CreateAudioPlayer)(void *, void **, void *, void *, SLuint32, const SLInterfaceID *, const SLboolean *);
    void *CreateAudioRecorder, *CreateMidiPlayer, *CreateListener, *Create3DGroup;
    SLresult (*CreateOutputMix)(void *, void **, SLuint32, const SLInterfaceID *, const SLboolean *);
    void *CreateMetadataExtractor, *CreateExtensionObject, *QueryNumSupportedInterfaces,
         *QuerySupportedInterfaces, *QueryNumSupportedExtensions, *QuerySupportedExtension, *IsExtensionSupported;
} SLEngineItf_;

typedef struct SLPlayItf_ {
    SLresult (*SetPlayState)(void *, SLuint32);
    SLresult (*GetPlayState)(void *, SLuint32 *);
    void *GetDuration, *GetPosition, *RegisterCallback, *SetCallbackEventsMask, *GetCallbackEventsMask,
         *SetMarkerPosition, *ClearMarkerPosition, *GetMarkerPosition, *SetPositionUpdatePeriod, *GetPositionUpdatePeriod;
} SLPlayItf_;

typedef struct { SLuint32 count, index; } BQState;
typedef void (*BQCallback)(void *bq, void *ctx);
typedef struct SLBufferQueueItf_ {
    SLresult (*Enqueue)(void *, const void *, SLuint32);
    SLresult (*Clear)(void *);
    SLresult (*GetState)(void *, BQState *);
    SLresult (*RegisterCallback)(void *, BQCallback, void *);
} SLBufferQueueItf_;

typedef struct SLConfigItf_ { void *SetConfiguration, *GetConfiguration; } SLConfigItf_;
typedef struct SLVolumeItf_ {
    SLresult (*SetVolumeLevel)(void *, int16_t);
    SLresult (*GetVolumeLevel)(void *, int16_t *);
    SLresult (*GetMaxVolumeLevel)(void *, int16_t *);
    SLresult (*SetMute)(void *, SLboolean);
    SLresult (*GetMute)(void *, SLboolean *);
    void *EnableStereoPosition, *IsEnabledStereoPosition, *SetStereoPosition, *GetStereoPosition;
} SLVolumeItf_;

/* ------------------------------------------------------------ state */
#define QMAX 32
struct Obj {
    const SLObjectItf_ *vt;     /* must be first: this IS the SLObjectItf */
    int kind;
    /* sub-interfaces (each starts with its vtable pointer) */
    struct { const void *vt; Obj *o; } engine, play, bq, config, vol;
    int16_t vol_mb; int muted;
    /* player state */
    SDL_AudioDeviceID dev;
    int rate, channels;
    pthread_mutex_t m;
    const uint8_t *qbuf[QMAX]; uint32_t qlen[QMAX];
    int qhead, qcount; uint32_t qoff; uint32_t qindex;
    BQCallback cb; void *cbctx;
    SLuint32 playing;
    Obj *self_ref;
};

static void sdl_fill(void *ud, Uint8 *out, int len) {
    Obj *p = ud;
    int filled = 0, notify = 0;
    pthread_mutex_lock(&p->m);
    while (filled < len && p->qcount > 0) {
        uint32_t avail = p->qlen[p->qhead] - p->qoff;
        uint32_t n = avail < (uint32_t)(len - filled) ? avail : (uint32_t)(len - filled);
        memcpy(out + filled, p->qbuf[p->qhead] + p->qoff, n);
        filled += n; p->qoff += n;
        if (p->qoff >= p->qlen[p->qhead]) {
            p->qhead = (p->qhead + 1) % QMAX; p->qcount--; p->qoff = 0; p->qindex++;
            notify++;
        }
    }
    BQCallback cb = p->cb; void *ctx = p->cbctx;
    int16_t vmb = p->vol_mb, mute = p->muted;
    pthread_mutex_unlock(&p->m);
    if (filled < len) memset(out + filled, 0, len - filled);
    if (mute) memset(out, 0, filled);
    else if (vmb < 0) {
        int g = (int)(65536.0 * pow(10.0, vmb / 2000.0)); int16_t *sm = (int16_t *)out;
        for (int i = 0; i < filled / 2; i++) sm[i] = (int16_t)(((int)sm[i] * g) >> 16);
    }
    while (notify-- > 0 && cb) cb(&p->bq, ctx);
}

/* --------------------------------------------- buffer queue interface */
static SLresult bq_enqueue(void *self, const void *buf, SLuint32 size) {
    Obj *p = ((struct { const void *vt; Obj *o; } *)self)->o;
    pthread_mutex_lock(&p->m);
    if (p->qcount >= QMAX) { pthread_mutex_unlock(&p->m); return 7; /* BUFFER_INSUFFICIENT */ }
    int tail = (p->qhead + p->qcount) % QMAX;
    p->qbuf[tail] = buf; p->qlen[tail] = size; p->qcount++;
    pthread_mutex_unlock(&p->m);
    return OK;
}
static SLresult bq_clear(void *self) {
    Obj *p = ((struct { const void *vt; Obj *o; } *)self)->o;
    pthread_mutex_lock(&p->m); p->qcount = 0; p->qoff = 0; pthread_mutex_unlock(&p->m);
    return OK;
}
static SLresult bq_state(void *self, BQState *s) {
    Obj *p = ((struct { const void *vt; Obj *o; } *)self)->o;
    pthread_mutex_lock(&p->m); s->count = p->qcount; s->index = p->qindex; pthread_mutex_unlock(&p->m);
    return OK;
}
static SLresult bq_register(void *self, BQCallback cb, void *ctx) {
    Obj *p = ((struct { const void *vt; Obj *o; } *)self)->o;
    pthread_mutex_lock(&p->m); p->cb = cb; p->cbctx = ctx; pthread_mutex_unlock(&p->m);
    return OK;
}
static const SLBufferQueueItf_ bq_vt = { bq_enqueue, bq_clear, bq_state, bq_register };

/* ------------------------------------------------------ play interface */
static SLresult play_set(void *self, SLuint32 st) {
    Obj *p = ((struct { const void *vt; Obj *o; } *)self)->o;
    p->playing = st; /* 1 stopped, 2 paused, 3 playing */
    if (p->dev) SDL_PauseAudioDevice(p->dev, st != 3);
    return OK;
}
static SLresult play_get(void *self, SLuint32 *st) {
    Obj *p = ((struct { const void *vt; Obj *o; } *)self)->o;
    *st = p->playing ? p->playing : 1;
    return OK;
}
static const SLPlayItf_ play_vt = { .SetPlayState = play_set, .GetPlayState = play_get };

static const SLConfigItf_ config_vt;
#define VOBJ(self) (((struct { const void *vt; Obj *o; } *)(self))->o)
static SLresult vol_set(void *s, int16_t mb) { Obj *p = VOBJ(s); pthread_mutex_lock(&p->m); p->vol_mb = mb > 0 ? 0 : mb; pthread_mutex_unlock(&p->m); return OK; }
static SLresult vol_get(void *s, int16_t *mb) { *mb = VOBJ(s)->vol_mb; return OK; }
static SLresult vol_max(void *s, int16_t *mb) { (void)s; *mb = 0; return OK; }
static SLresult vol_setmute(void *s, SLboolean m) { VOBJ(s)->muted = m ? 1 : 0; return OK; }
static SLresult vol_getmute(void *s, SLboolean *m) { *m = VOBJ(s)->muted; return OK; }
static const SLVolumeItf_ vol_vt = { vol_set, vol_get, vol_max, vol_setmute, vol_getmute, 0, 0, 0, 0 };

/* --------------------------------------------------- object interface */
static SLresult o_realize(void *s, SLboolean a) { (void)s; (void)a; return OK; }
static SLresult o_resume(void *s, SLboolean a) { (void)s; (void)a; return OK; }
static SLresult o_state(void *s, SLuint32 *st) { (void)s; *st = 2; /* REALIZED */ return OK; }
static SLresult o_cb(void *s, void *c, void *x) { (void)s; (void)c; (void)x; return OK; }
static void o_abort(void *s) { (void)s; }
static SLresult o_prio(void *s, SLint32 a, SLboolean b) { (void)s; (void)a; (void)b; return OK; }
static SLresult o_getprio(void *s, SLint32 *a, SLboolean *b) { (void)s; *a = 0; *b = 0; return OK; }
static SLresult o_loss(void *s, int16_t n, void *i, SLboolean e) { (void)s; (void)n; (void)i; (void)e; return OK; }

static void o_destroy(void *s) {
    Obj *o = s;
    if (o->dev) { SDL_CloseAudioDevice(o->dev); o->dev = 0; }
    free(o);
}

extern const SLEngineItf_ engine_vt;
static SLresult o_getif(void *s, SLInterfaceID id, void *out) {
    Obj *o = s;
    if (o->kind == K_ENGINE && id == SL_IID_ENGINE) { *(void **)out = &o->engine; return OK; }
    if (o->kind == K_PLAYER) {
        if (id == SL_IID_PLAY) { *(void **)out = &o->play; return OK; }
        if (id == SL_IID_ANDROIDSIMPLEBUFFERQUEUE) { *(void **)out = &o->bq; return OK; }
        if (id == SL_IID_VOLUME) { *(void **)out = &o->vol; return OK; }
        if (id == SL_IID_ANDROIDCONFIGURATION) { *(void **)out = &o->config; return OK; }
    }
    *(void **)out = NULL;
    return E_UNSUPPORTED;
}
static const SLObjectItf_ obj_vt = { o_realize, o_resume, o_state, o_getif, o_cb, o_abort, o_destroy, o_prio, o_getprio, o_loss };

static Obj *new_obj(int kind) {
    Obj *o = calloc(1, sizeof *o);
    o->vt = &obj_vt; o->kind = kind;
    o->engine.vt = &engine_vt; o->engine.o = o;
    o->play.vt = &play_vt; o->play.o = o;
    o->bq.vt = &bq_vt; o->bq.o = o;
    o->config.vt = &config_vt; o->config.o = o;
    o->vol.vt = &vol_vt; o->vol.o = o;
    pthread_mutex_init(&o->m, NULL);
    return o;
}

/* ------------------------------------------------------ engine interface */
typedef struct { SLuint32 type; void *loc, *fmt; } DS;               /* SLDataSource / SLDataSink */
typedef struct { SLuint32 type, channels, rate_mhz, bits, container, mask, endian; } PCMFmt;

static SLresult e_mix(void *self, void **out, SLuint32 n, const SLInterfaceID *i, const SLboolean *r) {
    (void)self; (void)n; (void)i; (void)r;
    *out = new_obj(K_MIX);
    return OK;
}
static SLresult e_player(void *self, void **out, void *src, void *sink, SLuint32 n, const SLInterfaceID *i, const SLboolean *r) {
    (void)self; (void)sink; (void)n; (void)i; (void)r;
    struct { void *loc; PCMFmt *fmt; } *ds = src;
    Obj *p = new_obj(K_PLAYER);
    p->rate = ds->fmt ? (int)(ds->fmt->rate_mhz / 1000) : 48000;
    p->channels = ds->fmt ? (int)ds->fmt->channels : 2;
    SDL_AudioSpec want, got;
    SDL_zero(want);
    want.freq = p->rate; want.format = AUDIO_S16SYS; want.channels = (Uint8)p->channels;
    want.samples = 1024; want.callback = sdl_fill; want.userdata = p;
    p->dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0); /* 0: no format change; SDL converts */
    if (!p->dev) { fprintf(stderr, "[opensl] SDL_OpenAudioDevice: %s (player will be silent)\n", SDL_GetError()); *out = p; return OK; }
    SDL_PauseAudioDevice(p->dev, 1);
    fprintf(stderr, "[opensl] player %d Hz, %d ch\n", p->rate, p->channels);
    *out = p;
    return OK;
}
const SLEngineItf_ engine_vt = { .CreateAudioPlayer = e_player, .CreateOutputMix = e_mix };

EXPORT SLresult slCreateEngine(void **out, SLuint32 nopt, const void *opt, SLuint32 nif, const SLInterfaceID *ids, const SLboolean *req) {
    (void)nopt; (void)opt; (void)nif; (void)ids; (void)req;
    *out = new_obj(K_ENGINE);
    return OK;
}
