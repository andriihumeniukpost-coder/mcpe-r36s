# Build for 32-bit ARM hard-float Linux (R36S / ArkOS userland).
#   cross:   make CROSS=arm-linux-gnueabihf-
#   native:  make   (on the device itself, needs gcc + libsdl2-dev:armhf)
CROSS   ?=
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SDLCFG  ?= sdl2-config
CFLAGS  ?= -O2 -g -fPIC -Wall -Iinclude -U_FORTIFY_SOURCE $(shell $(SDLCFG) --cflags)
B       := build

all: $(B)/mcpe $(B)/libs/libc.so $(B)/libs/libm.so $(B)/libs/liblog.so $(B)/libs/libandroid.so \
     $(B)/libs/libEGL.so $(B)/libs/libdl.so $(B)/libs/libGLESv1_CM.so $(B)/libs/libOpenSLES.so \
     $(B)/libs/libGLESv2.so $(B)/libs/libz.so $(B)/libs/libstdc++.so

$(B)/libs $(B)/obj:
	mkdir -p $@

# host launcher (also contains the fake JVM)
$(B)/mcpe: src/main.c src/jni_fake.c include/stb_image.h | $(B)
	$(CC) $(CFLAGS) -o $@ src/main.c src/jni_fake.c $(shell $(SDLCFG) --libs) -ldl -lpthread -lm \
	      -Wl,--no-as-needed -lgcc_s
$(B):
	mkdir -p $@

# Bionic -> glibc compat. Functions are written as bc_<name>; objcopy renames
# them to the real names so they are exported only from this shim.
$(B)/obj/bionic_compat.o: src/bionic_compat.c | $(B)/obj
	$(CC) $(CFLAGS) -fno-builtin -c -o $@ $<
	$(OBJCOPY) --redefine-syms=src/bionic_redef.txt $@
$(B)/libs/libc.so: $(B)/obj/bionic_compat.o | $(B)/libs
	$(CC) -shared -o $@ $< -Wl,-soname,libc.so -ldl -lpthread

$(B)/libs/libm.so: src/libm_shim.c | $(B)/libs
	$(CC) $(CFLAGS) -fno-builtin -shared -o $@ $< -Wl,-soname,libm.so -ldl
$(B)/libs/liblog.so: src/liblog.c | $(B)/libs
	$(CC) $(CFLAGS) -shared -o $@ $< -Wl,-soname,liblog.so
$(B)/libs/libandroid.so: src/libandroid.c | $(B)/libs
	$(CC) $(CFLAGS) -shared -o $@ $< -Wl,-soname,libandroid.so -lpthread
$(B)/libs/libEGL.so: src/libegl.c | $(B)/libs
	$(CC) $(CFLAGS) -shared -o $@ $< -Wl,-soname,libEGL.so $(shell $(SDLCFG) --libs)

$(B)/libs/libOpenSLES.so: src/libopensles.c | $(B)/libs
	$(CC) $(CFLAGS) -shared -o $@ $< -Wl,-soname,libOpenSLES.so $(shell $(SDLCFG) --libs) -lpthread

# Empty libraries the game lists in DT_NEEDED
$(B)/libs/libdl.so $(B)/libs/libGLESv1_CM.so $(B)/libs/libGLESv2.so $(B)/libs/libz.so: | $(B)/libs
	echo "" > $(B)/obj/empty.c
	$(CC) -shared -fPIC -o $@ $(B)/obj/empty.c -Wl,-soname,$(notdir $@)

# The game never imports GL/zlib symbols directly (GL comes via eglGetProcAddress),
# so empty libraries are enough. libfmod needs the C++ runtime: stub that pulls in the system one.
$(B)/libs/libstdc++.so: | $(B)/libs
	echo "" > $(B)/obj/empty.c
	$(CC) -shared -fPIC -o $@ $(B)/obj/empty.c -Wl,-soname,libstdc++.so -Wl,--no-as-needed -l:libstdc++.so.6

include/stb_image.h:
	sh tools/fetch_deps.sh

clean:
	rm -rf $(B)
.PHONY: all clean
