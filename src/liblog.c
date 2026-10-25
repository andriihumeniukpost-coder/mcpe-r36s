/* liblog.so replacement */
#include "android_min.h"
#include <stdarg.h>
#include <stdio.h>

EXPORT int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
    (void)prio;
    fprintf(stderr, "[%s] ", tag ? tag : "");
    int n = vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    return n;
}
EXPORT int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = __android_log_vprint(prio, tag, fmt, ap);
    va_end(ap);
    return n;
}
EXPORT int __android_log_write(int prio, const char *tag, const char *text) {
    (void)prio;
    return fprintf(stderr, "[%s] %s\n", tag ? tag : "", text);
}
