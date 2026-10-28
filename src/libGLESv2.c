/* libGLESv2.so replacement.
 * 1.19.x imports the GLES2 entry points directly (older builds went through eglGetProcAddress), and the game is
 * soft-float while this host is hard-float. Every entry point is therefore a tiny ARM trampoline that, on first
 * call, asks our own eglGetProcAddress (src/libegl.c) for the real function and then tail-jumps to it with the
 * argument registers untouched. eglGetProcAddress already hands out soft-float wrappers (w_gl*) for the few
 * functions with float arguments, and the raw driver function for the rest, so no calling-convention handling
 * is needed here and every tweak made in libegl.c (shader patching, scaling, overlays) applies to this path too.
 *
 * Generated list = the 82 gl* symbols libminecraftpe.so (1.19.22, armeabi-v7a) imports. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))
#define HIDDEN __attribute__((visibility("hidden")))

#define GL_FUNCS \
  X(glActiveTexture) \
  X(glAttachShader) \
  X(glBindBuffer) \
  X(glBindFramebuffer) \
  X(glBindRenderbuffer) \
  X(glBindTexture) \
  X(glBlendColor) \
  X(glBlendEquationSeparate) \
  X(glBlendFuncSeparate) \
  X(glBufferData) \
  X(glBufferSubData) \
  X(glCheckFramebufferStatus) \
  X(glClear) \
  X(glClearColor) \
  X(glClearDepthf) \
  X(glClearStencil) \
  X(glColorMask) \
  X(glCompileShader) \
  X(glCompressedTexImage2D) \
  X(glCompressedTexSubImage2D) \
  X(glCreateProgram) \
  X(glCreateShader) \
  X(glCullFace) \
  X(glDeleteBuffers) \
  X(glDeleteFramebuffers) \
  X(glDeleteProgram) \
  X(glDeleteRenderbuffers) \
  X(glDeleteShader) \
  X(glDeleteTextures) \
  X(glDepthFunc) \
  X(glDepthMask) \
  X(glDetachShader) \
  X(glDisable) \
  X(glDisableVertexAttribArray) \
  X(glDrawArrays) \
  X(glDrawElements) \
  X(glEnable) \
  X(glEnableVertexAttribArray) \
  X(glFlush) \
  X(glFramebufferRenderbuffer) \
  X(glFramebufferTexture2D) \
  X(glFrontFace) \
  X(glGenBuffers) \
  X(glGenFramebuffers) \
  X(glGenRenderbuffers) \
  X(glGenTextures) \
  X(glGenerateMipmap) \
  X(glGetActiveAttrib) \
  X(glGetActiveUniform) \
  X(glGetAttribLocation) \
  X(glGetError) \
  X(glGetFloatv) \
  X(glGetIntegerv) \
  X(glGetProgramInfoLog) \
  X(glGetProgramiv) \
  X(glGetShaderInfoLog) \
  X(glGetShaderPrecisionFormat) \
  X(glGetShaderiv) \
  X(glGetString) \
  X(glGetUniformLocation) \
  X(glLinkProgram) \
  X(glPixelStorei) \
  X(glPolygonOffset) \
  X(glReadPixels) \
  X(glRenderbufferStorage) \
  X(glScissor) \
  X(glShaderSource) \
  X(glStencilFuncSeparate) \
  X(glStencilOpSeparate) \
  X(glTexImage2D) \
  X(glTexParameterf) \
  X(glTexParameterfv) \
  X(glTexParameteri) \
  X(glTexSubImage2D) \
  X(glUniform1i) \
  X(glUniform1iv) \
  X(glUniform4fv) \
  X(glUniformMatrix3fv) \
  X(glUniformMatrix4fv) \
  X(glUseProgram) \
  X(glVertexAttribPointer) \
  X(glViewport)

/* set by the launcher: = eglGetProcAddress of our libEGL shim */
EXPORT void *(*shim_gl_getproc)(const char *name);

#define X(n) #n,
static const char *const gl_names[] = { GL_FUNCS };
#undef X
#define NGL (sizeof gl_names / sizeof *gl_names)
HIDDEN void *gl_slots[NGL];

static void gl_missing(void) { fprintf(stderr, "[glshim] call to an unresolved GL function\n"); fflush(stderr); abort(); }

HIDDEN void *gl_resolve(unsigned idx) {
    void *p = shim_gl_getproc ? shim_gl_getproc(gl_names[idx]) : NULL;
    if (!p) { fprintf(stderr, "[glshim] %s: not provided by the GL driver\n", gl_names[idx]); p = (void *)gl_missing; }
    gl_slots[idx] = p;
    return p;
}

/* 0: ip = &gl_slots[idx] (pc-relative, no text relocations); load; if set -> jump.
 * Slow path: preserve r0-r3 (arguments) and lr, resolve, restore, jump. */
#define TRAMP(n, idx) \
    __asm__(".arm\n.text\n.align 2\n.globl " #n "\n.type " #n ",%function\n" #n ":\n" \
            "  ldr ip, 1f\n" \
            "2: add ip, pc, ip\n" \
            "  ldr ip, [ip]\n" \
            "  cmp ip, #0\n" \
            "  bxne ip\n" \
            "  push {r0-r3, r4, lr}\n" \
            "  mov r0, #" #idx "\n" \
            "  bl gl_resolve\n" \
            "  mov ip, r0\n" \
            "  pop {r0-r3, r4, lr}\n" \
            "  bx ip\n" \
            "1: .word gl_slots + 4*" #idx " - (2b + 8)\n" \
            ".size " #n ", .-" #n "\n");
TRAMP(glActiveTexture, 0)
TRAMP(glAttachShader, 1)
TRAMP(glBindBuffer, 2)
TRAMP(glBindFramebuffer, 3)
TRAMP(glBindRenderbuffer, 4)
TRAMP(glBindTexture, 5)
TRAMP(glBlendColor, 6)
TRAMP(glBlendEquationSeparate, 7)
TRAMP(glBlendFuncSeparate, 8)
TRAMP(glBufferData, 9)
TRAMP(glBufferSubData, 10)
TRAMP(glCheckFramebufferStatus, 11)
TRAMP(glClear, 12)
TRAMP(glClearColor, 13)
TRAMP(glClearDepthf, 14)
TRAMP(glClearStencil, 15)
TRAMP(glColorMask, 16)
TRAMP(glCompileShader, 17)
TRAMP(glCompressedTexImage2D, 18)
TRAMP(glCompressedTexSubImage2D, 19)
TRAMP(glCreateProgram, 20)
TRAMP(glCreateShader, 21)
TRAMP(glCullFace, 22)
TRAMP(glDeleteBuffers, 23)
TRAMP(glDeleteFramebuffers, 24)
TRAMP(glDeleteProgram, 25)
TRAMP(glDeleteRenderbuffers, 26)
TRAMP(glDeleteShader, 27)
TRAMP(glDeleteTextures, 28)
TRAMP(glDepthFunc, 29)
TRAMP(glDepthMask, 30)
TRAMP(glDetachShader, 31)
TRAMP(glDisable, 32)
TRAMP(glDisableVertexAttribArray, 33)
TRAMP(glDrawArrays, 34)
TRAMP(glDrawElements, 35)
TRAMP(glEnable, 36)
TRAMP(glEnableVertexAttribArray, 37)
TRAMP(glFlush, 38)
TRAMP(glFramebufferRenderbuffer, 39)
TRAMP(glFramebufferTexture2D, 40)
TRAMP(glFrontFace, 41)
TRAMP(glGenBuffers, 42)
TRAMP(glGenFramebuffers, 43)
TRAMP(glGenRenderbuffers, 44)
TRAMP(glGenTextures, 45)
TRAMP(glGenerateMipmap, 46)
TRAMP(glGetActiveAttrib, 47)
TRAMP(glGetActiveUniform, 48)
TRAMP(glGetAttribLocation, 49)
TRAMP(glGetError, 50)
TRAMP(glGetFloatv, 51)
TRAMP(glGetIntegerv, 52)
TRAMP(glGetProgramInfoLog, 53)
TRAMP(glGetProgramiv, 54)
TRAMP(glGetShaderInfoLog, 55)
TRAMP(glGetShaderPrecisionFormat, 56)
TRAMP(glGetShaderiv, 57)
TRAMP(glGetString, 58)
TRAMP(glGetUniformLocation, 59)
TRAMP(glLinkProgram, 60)
TRAMP(glPixelStorei, 61)
TRAMP(glPolygonOffset, 62)
TRAMP(glReadPixels, 63)
TRAMP(glRenderbufferStorage, 64)
TRAMP(glScissor, 65)
TRAMP(glShaderSource, 66)
TRAMP(glStencilFuncSeparate, 67)
TRAMP(glStencilOpSeparate, 68)
TRAMP(glTexImage2D, 69)
TRAMP(glTexParameterf, 70)
TRAMP(glTexParameterfv, 71)
TRAMP(glTexParameteri, 72)
TRAMP(glTexSubImage2D, 73)
TRAMP(glUniform1i, 74)
TRAMP(glUniform1iv, 75)
TRAMP(glUniform4fv, 76)
TRAMP(glUniformMatrix3fv, 77)
TRAMP(glUniformMatrix4fv, 78)
TRAMP(glUseProgram, 79)
TRAMP(glVertexAttribPointer, 80)
TRAMP(glViewport, 81)
