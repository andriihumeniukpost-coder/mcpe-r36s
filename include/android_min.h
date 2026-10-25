/* Minimal re-declaration of the Android NDK types the game needs.
 * Layouts match the NDK headers (android/native_activity.h, android/input.h). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <jni.h>

/* The game (libminecraftpe.so) is built soft-float (floats travel in core
 * registers). Everything we export to it that takes/returns float/double
 * must use the base AAPCS convention, even though the host is hard-float. */
#if defined(__arm__)
#define SOFT __attribute__((pcs("aapcs")))
#else
#define SOFT
#endif
#define EXPORT __attribute__((visibility("default")))

typedef struct ANativeWindow ANativeWindow;
typedef struct AAssetManager AAssetManager;
typedef struct AInputQueue AInputQueue;
typedef struct ALooper ALooper;
typedef struct ANativeActivity ANativeActivity;
typedef struct { int32_t left, top, right, bottom; } ARect;

typedef struct ANativeActivityCallbacks {
    void (*onStart)(ANativeActivity*);
    void (*onResume)(ANativeActivity*);
    void *(*onSaveInstanceState)(ANativeActivity*, size_t*);
    void (*onPause)(ANativeActivity*);
    void (*onStop)(ANativeActivity*);
    void (*onDestroy)(ANativeActivity*);
    void (*onWindowFocusChanged)(ANativeActivity*, int);
    void (*onNativeWindowCreated)(ANativeActivity*, ANativeWindow*);
    void (*onNativeWindowResized)(ANativeActivity*, ANativeWindow*);
    void (*onNativeWindowRedrawNeeded)(ANativeActivity*, ANativeWindow*);
    void (*onNativeWindowDestroyed)(ANativeActivity*, ANativeWindow*);
    void (*onInputQueueCreated)(ANativeActivity*, AInputQueue*);
    void (*onInputQueueDestroyed)(ANativeActivity*, AInputQueue*);
    void (*onContentRectChanged)(ANativeActivity*, const ARect*);
    void (*onConfigurationChanged)(ANativeActivity*);
    void (*onLowMemory)(ANativeActivity*);
} ANativeActivityCallbacks;

struct ANativeActivity {
    ANativeActivityCallbacks *callbacks;
    JavaVM *vm;
    JNIEnv *env;
    jobject clazz;
    const char *internalDataPath;
    const char *externalDataPath;
    int32_t sdkVersion;
    void *instance;
    AAssetManager *assetManager;
    const char *obbPath;
};

/* Our own AInputEvent (opaque to the game, which only uses accessors). */
enum { EV_KEY = 1, EV_MOTION = 2 };
#define MAX_AXES 48
typedef struct AInputEvent {
    int type, source, device, action;
    int keycode, meta, repeat;
    int pointer_count;
    float x[2], y[2];
    int pointer_id[2];
    float axis[MAX_AXES];
} AInputEvent;

#define AINPUT_SOURCE_GAMEPAD  0x00000401
#define AINPUT_SOURCE_JOYSTICK 0x01000010
#define AINPUT_SOURCE_TOUCH    0x00001002

/* Axis ids (android/input.h) */
#define AXIS_X 0
#define AXIS_Y 1
#define AXIS_Z 11
#define AXIS_RZ 14
#define AXIS_HAT_X 15
#define AXIS_HAT_Y 16
#define AXIS_LTRIGGER 17
#define AXIS_RTRIGGER 18

/* Helpers exported by our libandroid.so, looked up with dlsym from main(). */
AAssetManager *shim_asset_manager_create(const char *root);
AInputQueue *shim_queue_create(void);
void shim_queue_push(AInputQueue *q, const AInputEvent *ev);
extern volatile int shim_finish_requested;
