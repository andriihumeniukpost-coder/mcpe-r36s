/* libandroid.so replacement: assets, looper, input queue, config, window.
 * Built as a shared library with soname "libandroid.so". */
#define _GNU_SOURCE
#include "android_min.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/mman.h>

volatile int shim_finish_requested;

/* ------------------------------------------------------------ assets */
struct AAssetManager { char root[512]; };
typedef struct AAsset { int fd; int64_t len, pos; void *map; } AAsset;
typedef struct AAssetDir { DIR *d; } AAssetDir;

EXPORT AAssetManager *shim_asset_manager_create(const char *root) {
    AAssetManager *m = calloc(1, sizeof *m);
    snprintf(m->root, sizeof m->root, "%s", root);
    return m;
}

/* Files are NOT read into RAM (1.19 has 300 MB of assets on a 1 GB device): positional reads on an fd, and a
 * read-only mmap only if the game asks for AAsset_getBuffer(). */
EXPORT AAsset *AAssetManager_open(AAssetManager *m, const char *name, int mode) {
    { static int n; if (n < 40) { n++; fprintf(stderr, "[android] AAssetManager_open %s\n", name); } }
    (void)mode;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", m->root, name);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return NULL; }
    AAsset *a = calloc(1, sizeof *a);
    a->fd = fd; a->len = st.st_size;
    return a;
}
EXPORT void AAsset_close(AAsset *a) {
    if (!a) return;
    if (a->map) munmap(a->map, a->len ? a->len : 1);
    close(a->fd); free(a);
}
EXPORT const void *AAsset_getBuffer(AAsset *a) {
    if (!a->map && a->len > 0) {
        void *p = mmap(NULL, a->len, PROT_READ, MAP_PRIVATE, a->fd, 0);
        a->map = p == MAP_FAILED ? NULL : p;
    }
    return a->map;
}
EXPORT int32_t AAsset_getLength(AAsset *a) { return a->len > 0x7fffffff ? 0x7fffffff : (int32_t)a->len; }
EXPORT int64_t AAsset_getLength64(AAsset *a) { return a->len; }
EXPORT int32_t AAsset_getRemainingLength(AAsset *a) { int64_t r = a->len - a->pos; return r > 0x7fffffff ? 0x7fffffff : (int32_t)r; }
EXPORT int64_t AAsset_getRemainingLength64(AAsset *a) { return a->len - a->pos; }
EXPORT int AAsset_read(AAsset *a, void *dst, size_t n) {
    int64_t left = a->len - a->pos;
    if ((int64_t)n > left) n = (size_t)left;
    size_t done = 0;
    while (done < n) {
        ssize_t r = pread(a->fd, (char *)dst + done, n - done, (off_t)(a->pos + done));
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) break;
        done += (size_t)r;
    }
    a->pos += (int64_t)done;
    return (int)done;
}
EXPORT int64_t AAsset_seek64(AAsset *a, int64_t off, int whence) {
    int64_t p = whence == SEEK_SET ? off : whence == SEEK_CUR ? a->pos + off : a->len + off;
    if (p < 0 || p > a->len) return -1;
    return a->pos = p;
}
EXPORT int32_t AAsset_seek(AAsset *a, int32_t off, int whence) { return (int32_t)AAsset_seek64(a, off, whence); }

EXPORT AAssetDir *AAssetManager_openDir(AAssetManager *m, const char *dir) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", m->root, dir);
    DIR *d = opendir(path);
    AAssetDir *ad = calloc(1, sizeof *ad);
    ad->d = d;
    return ad;
}
EXPORT const char *AAssetDir_getNextFileName(AAssetDir *ad) {
    struct dirent *e;
    if (!ad || !ad->d) return NULL;
    while ((e = readdir(ad->d))) {
        if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
        if (e->d_type == DT_DIR) continue; /* Android lists files only */
        return e->d_name;
    }
    return NULL;
}
EXPORT void AAssetDir_close(AAssetDir *ad) { if (ad) { if (ad->d) closedir(ad->d); free(ad); } }

/* ------------------------------------------------------ configuration */
EXPORT void *AConfiguration_new(void) { fprintf(stderr, "[android] AConfiguration_new\n"); return calloc(1, 64); }
EXPORT void AConfiguration_delete(void *c) { free(c); }
EXPORT void AConfiguration_fromAssetManager(void *c, AAssetManager *m) { (void)c; (void)m; }
EXPORT void AConfiguration_getLanguage(void *c, char *out) { (void)c; out[0] = 'e'; out[1] = 'n'; }
EXPORT void AConfiguration_getCountry(void *c, char *out) { (void)c; out[0] = 'U'; out[1] = 'S'; }

/* ---------------------------------------------------------- looper */
struct ALooper { int epfd; };
struct fdent { int fd, ident; void *data; };
static __thread ALooper *tl_looper;

EXPORT ALooper *ALooper_prepare(int opts) {
    { static int pn; if (pn < 5) { pn++; fprintf(stderr, "[android] ALooper_prepare\n"); } }
    (void)opts;
    if (!tl_looper) {
        tl_looper = calloc(1, sizeof *tl_looper);
        tl_looper->epfd = epoll_create1(0);
    }
    return tl_looper;
}
EXPORT int ALooper_addFd(ALooper *l, int fd, int ident, int events, void *cb, void *data) {
    fprintf(stderr, "[android] ALooper_addFd fd=%d ident=%d\n", fd, ident);
    (void)events; (void)cb; /* callbacks unsupported: the glue uses ident polling */
    struct fdent *e = calloc(1, sizeof *e);
    e->fd = fd; e->ident = ident; e->data = data;
    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = e };
    return epoll_ctl(l->epfd, EPOLL_CTL_ADD, fd, &ev) == 0 ? 1 : -1;
}
EXPORT int ALooper_pollAll(int timeout, int *outFd, int *outEvents, void **outData) {
    { static int n; if (n < 5) { n++; fprintf(stderr, "[android] ALooper_pollAll timeout=%d\n", timeout); } }
    ALooper *l = ALooper_prepare(0);
    struct epoll_event ev;
    int n = epoll_wait(l->epfd, &ev, 1, timeout);
    if (n == 0) return -3;                /* ALOOPER_POLL_TIMEOUT */
    if (n < 0) return errno == EINTR ? -1 : -4;
    struct fdent *e = ev.data.ptr;
    if (outFd) *outFd = e->fd;
    if (outEvents) *outEvents = 1;
    if (outData) *outData = e->data;
    return e->ident;
}
EXPORT int ALooper_pollOnce(int timeout, int *a, int *b, void **c) { return ALooper_pollAll(timeout, a, b, c); }

/* ------------------------------------------------------ input queue */
#define QCAP 512
struct AInputQueue {
    int rfd, wfd;
    pthread_mutex_t m;
    AInputEvent *q[QCAP];
    int head, tail;
    ALooper *attached;
};
EXPORT AInputQueue *shim_queue_create(void) {
    AInputQueue *q = calloc(1, sizeof *q);
    int p[2];
    if (pipe2(p, O_NONBLOCK | O_CLOEXEC)) abort();
    q->rfd = p[0]; q->wfd = p[1];
    pthread_mutex_init(&q->m, NULL);
    return q;
}
EXPORT void shim_queue_push(AInputQueue *q, const AInputEvent *ev) {
    pthread_mutex_lock(&q->m);
    int next = (q->tail + 1) % QCAP;
    if (next != q->head) {
        AInputEvent *c = malloc(sizeof *c);
        *c = *ev;
        q->q[q->tail] = c;
        q->tail = next;
        char b = 1;
        if (write(q->wfd, &b, 1) < 0) { /* pipe full: event still queued */ }
    }
    pthread_mutex_unlock(&q->m);
}
EXPORT void AInputQueue_attachLooper(AInputQueue *q, ALooper *l, int ident, void *cb, void *data) {
    q->attached = l;
    ALooper_addFd(l, q->rfd, ident, 1, cb, data);
}
EXPORT void AInputQueue_detachLooper(AInputQueue *q) {
    if (q->attached) epoll_ctl(q->attached->epfd, EPOLL_CTL_DEL, q->rfd, NULL);
    q->attached = NULL;
}
EXPORT int32_t AInputQueue_hasEvents(AInputQueue *q) { return q->head != q->tail; }
EXPORT int32_t AInputQueue_getEvent(AInputQueue *q, AInputEvent **out) {
    int r = -1;
    pthread_mutex_lock(&q->m);
    if (q->head != q->tail) {
        *out = q->q[q->head];
        q->head = (q->head + 1) % QCAP;
        char b;
        if (read(q->rfd, &b, 1) < 0) { }
        r = 0;
    }
    pthread_mutex_unlock(&q->m);
    return r;
}
EXPORT int32_t AInputQueue_preDispatchEvent(AInputQueue *q, AInputEvent *e) { (void)q; (void)e; return 0; }
EXPORT void AInputQueue_finishEvent(AInputQueue *q, AInputEvent *e, int handled) { (void)q; (void)handled; free(e); }

/* ------------------------------------------------- event accessors */
EXPORT int32_t AInputEvent_getType(const AInputEvent *e) { return e->type; }
EXPORT int32_t AInputEvent_getSource(const AInputEvent *e) { return e->source; }
EXPORT int32_t AInputEvent_getDeviceId(const AInputEvent *e) { return e->device; }
EXPORT int32_t AKeyEvent_getAction(const AInputEvent *e) { return e->action; }
EXPORT int32_t AKeyEvent_getKeyCode(const AInputEvent *e) { return e->keycode; }
EXPORT int32_t AKeyEvent_getMetaState(const AInputEvent *e) { return e->meta; }
EXPORT int32_t AKeyEvent_getRepeatCount(const AInputEvent *e) { return e->repeat; }
EXPORT int32_t AMotionEvent_getAction(const AInputEvent *e) { return e->action; }
EXPORT size_t AMotionEvent_getPointerCount(const AInputEvent *e) { return e->pointer_count; }
EXPORT int32_t AMotionEvent_getPointerId(const AInputEvent *e, size_t i) { return e->pointer_id[i & 1]; }
EXPORT SOFT float AMotionEvent_getX(const AInputEvent *e, size_t i) { return e->x[i & 1]; }
EXPORT SOFT float AMotionEvent_getY(const AInputEvent *e, size_t i) { return e->y[i & 1]; }
EXPORT SOFT float AMotionEvent_getRawX(const AInputEvent *e, size_t i) { return e->x[i & 1]; }
EXPORT SOFT float AMotionEvent_getRawY(const AInputEvent *e, size_t i) { return e->y[i & 1]; }
EXPORT SOFT float AMotionEvent_getAxisValue(const AInputEvent *e, int32_t axis, size_t i) {
    (void)i;
    return (axis >= 0 && axis < MAX_AXES) ? e->axis[axis] : 0.0f;
}

/* ------------------------------------------------ window / activity */
extern volatile int host_screen_w, host_screen_h;      /* set by the launcher (drawable size) */
EXPORT int32_t ANativeWindow_getWidth(ANativeWindow *w) { (void)w; return host_screen_w ? host_screen_w : 640; }
EXPORT int32_t ANativeWindow_getHeight(ANativeWindow *w) { (void)w; return host_screen_h ? host_screen_h : 480; }
EXPORT int32_t AMotionEvent_getButtonState(const AInputEvent *e) { (void)e; return 0; }
EXPORT int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t a, int32_t b, int32_t c) {
    (void)w; (void)a; (void)b; (void)c;
    return 0;
}
EXPORT void ANativeActivity_finish(ANativeActivity *a) { (void)a; shim_finish_requested = 1; }
