/* A tiny fake JVM. The game calls back into Java (MainActivity helpers,
 * HardwareInformation, Build fields, ...) through JNIEnv. We answer the few
 * calls that matter by method *name* and log everything unknown so the port can
 * be completed iteratively: run, read "[jni] unhandled" lines, add a case. */
#define _GNU_SOURCE
#include "android_min.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

const char *g_assets_root = ".";

enum { K_CLASS, K_STRING, K_INTARR, K_BYTEARR, K_OBJARR, K_OBJ };
typedef struct jobj { int kind; const char *name; char *str; int len; void *data; } jobj;
typedef struct meth { const char *name, *sig; } meth;
typedef union { jobject l; jint i; jlong j; jfloat f; jdouble d; jboolean z; } JV;

static jobj *mk(int kind, const char *name) {
    jobj *o = calloc(1, sizeof *o);
    o->kind = kind; o->name = name;
    return o;
}
static jstring mkstr(const char *s) { jobj *o = mk(K_STRING, "java/lang/String"); o->str = strdup(s ? s : ""); return (jstring)o; }

/* ------------------------------------------------ file helpers */
static unsigned char *slurp(const char *rel, long *n) {
    char path[1024];
    if (rel[0] == '/') snprintf(path, sizeof path, "%s", rel);
    else snprintf(path, sizeof path, "%s/%s", g_assets_root, rel);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(*n ? *n : 1);
    if (fread(b, 1, *n, f) != (size_t)*n) { free(b); fclose(f); return NULL; }
    fclose(f);
    return b;
}

/* Java side returned int[] { width, height, ARGB pixels... }.
 * TODO(verify against classes.dex disassembly: MainActivity.getImageData). */
static jobject java_getImageData(const char *name) {
    long n; unsigned char *file = slurp(name, &n);
    if (!file) { fprintf(stderr, "[jni] getImageData: cannot read %s\n", name); return NULL; }
    int w, h, c;
    unsigned char *px = stbi_load_from_memory(file, (int)n, &w, &h, &c, 4);
    free(file);
    if (!px) { fprintf(stderr, "[jni] getImageData: bad PNG %s\n", name); return NULL; }
    jobj *o = mk(K_INTARR, "[I");
    o->len = 2 + w * h;
    int32_t *d = malloc(sizeof(int32_t) * o->len);
    d[0] = w; d[1] = h;
    for (int i = 0; i < w * h; i++) {
        unsigned char *p = px + 4 * i;
        d[2 + i] = (int32_t)(((uint32_t)p[3] << 24) | (p[0] << 16) | (p[1] << 8) | p[2]);
    }
    stbi_image_free(px);
    o->data = d;
    return (jobject)o;
}
static jobject java_getFileDataBytes(const char *name) {
    long n; unsigned char *b = slurp(name, &n);
    if (!b) return NULL;
    jobj *o = mk(K_BYTEARR, "[B"); o->len = (int)n; o->data = b;
    return (jobject)o;
}

/* ------------------------------------------ method dispatch by name */
static void log_once(const char *kind, const char *name, const char *sig) {
    static const char *seen[256]; static int ns;
    for (int i = 0; i < ns; i++) if (seen[i] == name) return;
    if (ns < 256) seen[ns++] = name;
    fprintf(stderr, "[jni] %s %s %s\n", kind, name, sig);
}

static int which_dir;
static JV run(meth *m, jvalue *a) {
    JV r; memset(&r, 0, sizeof r);
    const char *n = m->name;
    log_once("call", n, m->sig);
    if (!strcmp(n, "getDateString")) { char b[64]; time_t t = time(NULL); strftime(b, sizeof b, "%Y-%m-%d", localtime(&t)); r.l = mkstr(b); }
    else if (!strcmp(n, "getImageData") && a[0].l) r.l = java_getImageData(((jobj *)a[0].l)->str);
    else if (!strcmp(n, "getFileDataBytes") && a[0].l) r.l = java_getFileDataBytes(((jobj *)a[0].l)->str);
    else if (!strcmp(n, "isNetworkEnabled")) r.z = 0;
    else if (!strcmp(n, "getExternalStoragePath")) { r.l = mkstr(getenv("MCPE_DATADIR")); }
    else if (!strcmp(n, "getFilesDir")) { which_dir = 0; }
    else if (!strcmp(n, "getCacheDir")) { which_dir = 1; }
    else if (!strcmp(n, "getPath") || !strcmp(n, "getAbsolutePath")) {
        char b[1300]; snprintf(b, sizeof b, "%s%s", getenv("MCPE_DATADIR") ? getenv("MCPE_DATADIR") : "", which_dir ? "/cache" : "/files"); r.l = mkstr(b); }
    else if (!strcmp(n, "getKeyFromKeyCode")) r.i = 0;
    else if (!strcmp(n, "getUserInputString")) { jobj *o = mk(K_OBJARR, "[Ljava/lang/String;"); o->len = 0; r.l = (jobject)o; }
    /* Calls that are pure UI side effects: ignore silently */
    else if (!strcmp(n, "displayDialog") || !strcmp(n, "showKeyboard") || !strcmp(n, "hideKeyboard") ||
             !strcmp(n, "updateTextboxText") || !strcmp(n, "setIsPowerVR") || !strcmp(n, "saveScreenshot") ||
             !strcmp(n, "pickImage") || !strcmp(n, "startActivity") || !strcmp(n, "updateLocalization") ||
             !strcmp(n, "setEnvironment") || !strcmp(n, "queryProducts") || !strcmp(n, "LogCLL") ||
             !strcmp(n, "<init>")) { }
    else if (m->sig[strlen(m->sig) - 1] == ';' && strstr(m->sig, "Ljava/lang/String;") && strchr(m->sig, ')') && !strcmp(strchr(m->sig, ')') + 1, "Ljava/lang/String;")) {
        log_once("string-method", n, m->sig); r.l = mkstr("");
    }
    else log_once("method", n, m->sig);
    return r;
}

static void fetch_args(meth *m, va_list ap, jvalue *a) {
    const char *p = m->sig + 1; int i = 0;
    memset(a, 0, 8 * sizeof *a);
    while (*p && *p != ')' && i < 8) {
        switch (*p) {
            case 'Z': case 'B': case 'C': case 'S': case 'I': a[i++].i = va_arg(ap, int); p++; break;
            case 'J': a[i++].j = va_arg(ap, long long); p++; break;
            case 'F': a[i++].f = (float)va_arg(ap, double); p++; break;
            case 'D': a[i++].d = va_arg(ap, double); p++; break;
            case '[': while (*p == '[') p++; if (*p == 'L') while (*p != ';') p++; p++; a[i++].l = va_arg(ap, jobject); break;
            case 'L': while (*p != ';') p++; p++; a[i++].l = va_arg(ap, jobject); break;
            default: p++;
        }
    }
}

#define FAM(NAME, RET, FIELD, ATTR) \
    ATTR static RET NAME(JNIEnv *e, jobject o, jmethodID m, ...) { (void)e; (void)o; va_list ap; va_start(ap, m); jvalue a[8]; fetch_args((meth *)m, ap, a); va_end(ap); return run((meth *)m, a).FIELD; } \
    ATTR static RET NAME##V(JNIEnv *e, jobject o, jmethodID m, va_list ap) { (void)e; (void)o; jvalue a[8]; fetch_args((meth *)m, ap, a); return run((meth *)m, a).FIELD; } \
    ATTR static RET NAME##A(JNIEnv *e, jobject o, jmethodID m, const jvalue *args) { (void)e; (void)o; jvalue a[8]; memcpy(a, args, sizeof a); return run((meth *)m, a).FIELD; }
FAM(C_Object, jobject, l, )
FAM(C_Int, jint, i, )
FAM(C_Boolean, jboolean, z, )
FAM(C_Long, jlong, j, )
FAM(C_Float, jfloat, f, SOFT)
FAM(C_Double, jdouble, d, SOFT)
static void C_Void(JNIEnv *e, jobject o, jmethodID m, ...) { (void)e; (void)o; va_list ap; va_start(ap, m); jvalue a[8]; fetch_args((meth *)m, ap, a); va_end(ap); run((meth *)m, a); }
static void C_VoidV(JNIEnv *e, jobject o, jmethodID m, va_list ap) { (void)e; (void)o; jvalue a[8]; fetch_args((meth *)m, ap, a); run((meth *)m, a); }
static void C_VoidA(JNIEnv *e, jobject o, jmethodID m, const jvalue *args) { (void)e; (void)o; jvalue a[8]; memcpy(a, args, sizeof a); run((meth *)m, a); }

/* ------------------------------------------------ misc JNI calls */
static jint J_GetVersion(JNIEnv *e) { (void)e; return JNI_VERSION_1_6; }
static jclass J_FindClass(JNIEnv *e, const char *n) { (void)e; fprintf(stderr, "[jni] FindClass %s\n", n); return (jclass)mk(K_CLASS, strdup(n)); }
static jclass J_GetObjectClass(JNIEnv *e, jobject o) { (void)e; (void)o; return (jclass)mk(K_CLASS, "java/lang/Object"); }
static jmethodID J_GetMethodID(JNIEnv *e, jclass c, const char *n, const char *s) { (void)e; (void)c; meth *m = malloc(sizeof *m); m->name = strdup(n); m->sig = strdup(s); return (jmethodID)m; }
static jobject J_NewGlobalRef(JNIEnv *e, jobject o) { (void)e; return o; }
static void J_DeleteRef(JNIEnv *e, jobject o) { (void)e; (void)o; }
static jboolean J_False(JNIEnv *e) { (void)e; return 0; }
static void J_Void(JNIEnv *e) { (void)e; }
static jobject J_Null(JNIEnv *e) { (void)e; return NULL; }
static jobject J_NewObjectV(JNIEnv *e, jclass c, jmethodID m, va_list a) { (void)e; (void)m; (void)a; return (jobject)mk(K_OBJ, ((jobj *)c)->name); }
static jobject J_NewObjectA(JNIEnv *e, jclass c, jmethodID m, const jvalue *a) { (void)e; (void)m; (void)a; return (jobject)mk(K_OBJ, ((jobj *)c)->name); }
static jint J_PushLocalFrame(JNIEnv *e, jint n) { (void)e; (void)n; return 0; }
static jobject J_PopLocalFrame(JNIEnv *e, jobject r) { (void)e; return r; }
static jobject J_NewObject(JNIEnv *e, jclass c, jmethodID m, ...) { (void)e; (void)m; return (jobject)mk(K_OBJ, ((jobj *)c)->name); }

static jstring J_NewStringUTF(JNIEnv *e, const char *s) { (void)e; return mkstr(s); }
static const char *J_GetStringUTFChars(JNIEnv *e, jstring s, jboolean *c) { (void)e; if (c) *c = 0; return s ? ((jobj *)s)->str : ""; }
static void J_ReleaseStringUTFChars(JNIEnv *e, jstring s, const char *c) { (void)e; (void)s; (void)c; }
static jsize J_GetStringUTFLength(JNIEnv *e, jstring s) { (void)e; return s ? (jsize)strlen(((jobj *)s)->str) : 0; }

static jsize J_GetArrayLength(JNIEnv *e, jarray a) { (void)e; return a ? ((jobj *)a)->len : 0; }
static jintArray J_NewIntArray(JNIEnv *e, jsize n) { (void)e; jobj *o = mk(K_INTARR, "[I"); o->len = n; o->data = calloc(n ? n : 1, 4); return (jintArray)o; }
static jbyteArray J_NewByteArray(JNIEnv *e, jsize n) { (void)e; jobj *o = mk(K_BYTEARR, "[B"); o->len = n; o->data = calloc(n ? n : 1, 1); return (jbyteArray)o; }
static void J_GetIntArrayRegion(JNIEnv *e, jintArray a, jsize s, jsize l, jint *b) { (void)e; memcpy(b, (int32_t *)((jobj *)a)->data + s, l * 4); }
static void J_SetIntArrayRegion(JNIEnv *e, jintArray a, jsize s, jsize l, const jint *b) { (void)e; memcpy((int32_t *)((jobj *)a)->data + s, b, l * 4); }
static void J_GetByteArrayRegion(JNIEnv *e, jbyteArray a, jsize s, jsize l, jbyte *b) { (void)e; memcpy(b, (char *)((jobj *)a)->data + s, l); }
static void J_SetByteArrayRegion(JNIEnv *e, jbyteArray a, jsize s, jsize l, const jbyte *b) { (void)e; memcpy((char *)((jobj *)a)->data + s, b, l); }
static void *J_GetArrayElements(JNIEnv *e, jarray a, jboolean *c) { (void)e; if (c) *c = 0; return ((jobj *)a)->data; }
static void J_ReleaseArrayElements(JNIEnv *e, jarray a, void *p, jint m) { (void)e; (void)a; (void)p; (void)m; }
static jobjectArray J_NewObjectArray(JNIEnv *e, jsize n, jclass c, jobject init) { (void)e; (void)c; (void)init; jobj *o = mk(K_OBJARR, "[Ljava/lang/Object;"); o->len = n; o->data = calloc(n ? n : 1, sizeof(void *)); return (jobjectArray)o; }
static jobject J_GetObjectArrayElement(JNIEnv *e, jobjectArray a, jsize i) { (void)e; return ((jobject *)((jobj *)a)->data)[i]; }
static void J_SetObjectArrayElement(JNIEnv *e, jobjectArray a, jsize i, jobject v) { (void)e; ((jobject *)((jobj *)a)->data)[i] = v; }

static jfieldID J_GetFieldID(JNIEnv *e, jclass c, const char *n, const char *s) { return (jfieldID)J_GetMethodID(e, c, n, s); }
static jint J_GetStaticIntField(JNIEnv *e, jclass c, jfieldID f) { (void)e; (void)c; return !strcmp(((meth *)f)->name, "SDK_INT") ? 21 : 0; }
static jobject J_GetStaticObjectField(JNIEnv *e, jclass c, jfieldID f) {
    (void)e; (void)c;
    const char *n = ((meth *)f)->name;
    if (!strcmp(n, "MODEL")) return mkstr("R36S");
    if (!strcmp(n, "MANUFACTURER")) return mkstr("Generic");
    if (!strcmp(n, "RELEASE")) return mkstr("5.0");
    return mkstr("unknown");
}
static jint J_RegisterNatives(JNIEnv *e, jclass c, const JNINativeMethod *m, jint n) { (void)e; (void)c; (void)m; (void)n; return 0; }

#include "jni_slots.h"

static struct JNINativeInterface_ iface;
static JNIEnv env_obj = &iface;
JNIEnv *fake_env(void);
static jint V_Attach(JavaVM *vm, void **penv, void *args) { (void)vm; (void)args; fake_env(); *penv = &env_obj; return JNI_OK; }
static jint V_GetEnv(JavaVM *vm, void **penv, jint v) { (void)vm; (void)v; fake_env(); *penv = &env_obj; return JNI_OK; }
static jint V_Detach(JavaVM *vm) { (void)vm; return JNI_OK; }
static jint V_Destroy(JavaVM *vm) { (void)vm; return JNI_OK; }
static struct JNIInvokeInterface_ vm_iface = {
    .DestroyJavaVM = V_Destroy, .AttachCurrentThread = V_Attach, .DetachCurrentThread = V_Detach,
    .GetEnv = V_GetEnv, .AttachCurrentThreadAsDaemon = V_Attach,
};
static JavaVM vm_obj = &vm_iface;

JNIEnv *fake_env(void);
JNIEnv *fake_env(void) {
    static int init;
    if (!init) {
        void **t = (void **)&iface;
        for (size_t i = 0; i < sizeof iface / sizeof(void *) && i < sizeof jni_unimpl_fns / sizeof(void *); i++) t[i] = jni_unimpl_fns[i];
        iface.GetVersion = J_GetVersion;
        iface.FindClass = J_FindClass;
        iface.GetObjectClass = J_GetObjectClass;
        iface.GetMethodID = J_GetMethodID; iface.GetStaticMethodID = J_GetMethodID;
        iface.NewGlobalRef = J_NewGlobalRef; iface.NewLocalRef = J_NewGlobalRef;
        iface.DeleteGlobalRef = J_DeleteRef; iface.DeleteLocalRef = J_DeleteRef;
        iface.ExceptionCheck = J_False; iface.ExceptionClear = J_Void; iface.ExceptionDescribe = J_Void; iface.ExceptionOccurred = J_Null;
        iface.NewObject = J_NewObject; iface.NewObjectV = J_NewObjectV; iface.NewObjectA = J_NewObjectA;
        iface.PushLocalFrame = J_PushLocalFrame; iface.PopLocalFrame = J_PopLocalFrame;
        iface.CallObjectMethod = C_Object; iface.CallObjectMethodV = C_ObjectV; iface.CallObjectMethodA = C_ObjectA;
        iface.CallIntMethod = C_Int; iface.CallIntMethodV = C_IntV; iface.CallIntMethodA = C_IntA;
        iface.CallBooleanMethod = C_Boolean; iface.CallBooleanMethodV = C_BooleanV; iface.CallBooleanMethodA = C_BooleanA;
        iface.CallLongMethod = C_Long; iface.CallLongMethodV = C_LongV; iface.CallLongMethodA = C_LongA;
        iface.CallFloatMethod = C_Float; iface.CallFloatMethodV = C_FloatV; iface.CallFloatMethodA = C_FloatA;
        iface.CallDoubleMethod = C_Double; iface.CallDoubleMethodV = C_DoubleV; iface.CallDoubleMethodA = C_DoubleA;
        iface.CallVoidMethod = C_Void; iface.CallVoidMethodV = C_VoidV; iface.CallVoidMethodA = C_VoidA;
        iface.CallStaticObjectMethod = (void *)C_Object; iface.CallStaticObjectMethodV = (void *)C_ObjectV; iface.CallStaticObjectMethodA = (void *)C_ObjectA;
        iface.CallStaticIntMethod = (void *)C_Int; iface.CallStaticIntMethodV = (void *)C_IntV; iface.CallStaticIntMethodA = (void *)C_IntA;
        iface.CallStaticBooleanMethod = (void *)C_Boolean; iface.CallStaticBooleanMethodV = (void *)C_BooleanV; iface.CallStaticBooleanMethodA = (void *)C_BooleanA;
        iface.CallStaticLongMethod = (void *)C_Long; iface.CallStaticLongMethodV = (void *)C_LongV; iface.CallStaticLongMethodA = (void *)C_LongA;
        iface.CallStaticFloatMethod = (void *)C_Float; iface.CallStaticFloatMethodV = (void *)C_FloatV; iface.CallStaticFloatMethodA = (void *)C_FloatA;
        iface.CallStaticVoidMethod = (void *)C_Void; iface.CallStaticVoidMethodV = (void *)C_VoidV; iface.CallStaticVoidMethodA = (void *)C_VoidA;
        iface.NewStringUTF = J_NewStringUTF; iface.GetStringUTFChars = J_GetStringUTFChars;
        iface.ReleaseStringUTFChars = J_ReleaseStringUTFChars; iface.GetStringUTFLength = J_GetStringUTFLength;
        iface.GetArrayLength = J_GetArrayLength;
        iface.NewIntArray = J_NewIntArray; iface.NewByteArray = J_NewByteArray;
        iface.GetIntArrayRegion = J_GetIntArrayRegion; iface.SetIntArrayRegion = J_SetIntArrayRegion;
        iface.GetByteArrayRegion = J_GetByteArrayRegion; iface.SetByteArrayRegion = J_SetByteArrayRegion;
        iface.GetIntArrayElements = (void *)J_GetArrayElements; iface.ReleaseIntArrayElements = (void *)J_ReleaseArrayElements;
        iface.GetByteArrayElements = (void *)J_GetArrayElements; iface.ReleaseByteArrayElements = (void *)J_ReleaseArrayElements;
        iface.NewObjectArray = J_NewObjectArray; iface.GetObjectArrayElement = J_GetObjectArrayElement; iface.SetObjectArrayElement = J_SetObjectArrayElement;
        iface.GetFieldID = J_GetFieldID; iface.GetStaticFieldID = J_GetFieldID;
        iface.GetStaticIntField = J_GetStaticIntField; iface.GetStaticObjectField = J_GetStaticObjectField;
        iface.RegisterNatives = J_RegisterNatives;
        init = 1;
    }
    return &env_obj;
}
JavaVM *fake_vm(void) { return &vm_obj; }
jobject fake_activity_obj(void) { static jobject o; if (!o) o = (jobject)mk(K_OBJ, "com/mojang/minecraftpe/MainActivity"); return o; }
