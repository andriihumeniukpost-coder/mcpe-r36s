# Minecraft PE 0.16.0 → R36S (ArkOS)

Запускает **оригинальный** `libminecraftpe.so` (ARMv7, Android) на glibc-Linux
(ArkOS, 32-bit userland) — исходников нет, поэтому порт = слой совместимости.

```
mcpe (SDL2 launcher + fake JVM)            ← src/main.c, src/jni_fake.c
 ├─ libc.so      Bionic→glibc: pthread, FILE (__sF), stat, dirent, sysconf, ctype
 ├─ libm.so      soft-float → hard-float обёртки для libm
 ├─ libandroid.so  AAssetManager, ALooper, AInputQueue, AMotionEvent/AKeyEvent
 ├─ libEGL.so    EGL поверх SDL2 + soft-float обёртки для GL (glUniform1f…)
 ├─ liblog.so, libdl.so/libGLESv1_CM.so/libOpenSLES.so (пустые)
 └─ libminecraftpe.so + libfmod.so (из APK, dlopen с RTLD_DEEPBIND)
```
Ключевые факты об игре: NativeActivity + android_native_app_glue, GLES2 (функции GL
берутся через `eglGetProcAddress`), **soft-float ABI** (поэтому все float-границы обёрнуты).

## Сборка
GitHub Actions (`.github/workflows/build.yml`) собирает в armhf Ubuntu 20.04.
Локально на устройстве: `sh tools/fetch_deps.sh && make`.

## Установка на ArkOS
1. `pack/prepare_game.sh minecraft-0-16-0.apk out/mcpe` — достаёт `libminecraftpe.so`, `libfmod.so`, `assets/`.
2. Скопировать `build/mcpe` и `build/libs/*` в `out/mcpe/` (libs — в `out/mcpe/libs/`).
3. `pack/Minecraft.sh` + папку `mcpe/` → `roms/ports/`. Лог: `mcpe/log.txt`.

## Статус: каркас, на железе НЕ запускался
Не проверено: загрузка .so, JNI, звук, управление. Ожидаемый цикл отладки — запуск и чтение
`log.txt`: строки `[jni] unhandled …` показывают, какие Java-вызовы надо реализовать
(`src/jni_fake.c`, функция `run`).

### Известные пробелы / TODO
1. **Звук**: звуки лежат в `.fsb` (FMOD банки), их декодирует оригинальный `libfmod.so`; мы даём ему
   `libOpenSLES.so` (src/libopensles.c) поверх SDL audio. Не проверено: FMOD может дополнительно
   вызывать Java (`org/fmod/*`) — смотреть `[jni] unhandled` в логе.
2. `getImageData` возвращает `int[]{w,h,ARGB…}` — формат нужно сверить с `classes.dex`.
3. Регистрация геймпада: игра ждёт `JellyBeanDeviceManager.onInputDeviceAddedNative` — вызов не сделан,
   сигнатуру надо уточнить.
4. `sigsetjmp/siglongjmp` (jmp_buf у Bionic меньше) и `getaddrinfo` (другой layout struct) не обёрнуты.
5. Производительность: ОЗУ 1 ГБ, Mali-G31. Вероятно понадобится уменьшение дальности прорисовки/разрешения.
6. Если `libgcc_s` не экспортирует нужные `__aeabi_*` — добавить их в `libm_shim.c`.

`include/jni.h`, `jni_md.h` — из OpenJDK (GPLv2 + Classpath Exception), только для сборки.

## Minecraft PE 0.8.1 (экспериментально)
Положите `minecraft-0-8-1-*.apk` в `mcpe/` (вместо 0.16.0 — лаунчер сам перепакует при смене APK) и запустите `Minecraft.sh`.
Режим включается автоматически, если в APK нет `libfmod.so`: GLES1.1 эмулируется поверх GLES2 (`src/libgles1.c`), звук идёт через OpenSL buffer queue.
Управление: левый стик/D-pad — движение, A=Cross, B=Circle(назад), X=Square, Y=Triangle, L1/R1, START/SELECT;
правый стик — указатель для меню (A = клик) или обзор; **R3 переключает** указатель/обзор. L3+R3 — оверлей, SELECT+L1/R1 — масштаб рендера.

## Minecraft PE 1.2.x (экспериментально, папка mcpe115 / `pack/Minecraft115.sh`)
1.2.10.2 (Bedrock-движок) отличается от 1.1.5 импортами, которых раньше не было:
- `AAsset_getLength64`, `AAsset_getRemainingLength64`, `AAsset_seek64` — теперь реализованы в `src/libandroid.c` как настоящие
  `int64_t`-функции (на ARM EABI off64_t возвращается в r0:r1, поэтому подмена на 32-битные имена в dynstr — не вариант).
  **Патчить `libminecraftpe.so` больше не нужно** — берётся оригинал из APK.
- `strlcpy`/`strlcat` — в glibc 2.30 (ArkOS) их нет, добавлены в `src/bionic_compat.c`.
Лаунчер сам перепаковывает `libminecraftpe.so`/`libfmod.so`/`assets` при смене APK (маркер `libs/.apk`).
