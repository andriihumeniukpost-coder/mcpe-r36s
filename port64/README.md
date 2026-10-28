# port64 — Minecraft Bedrock 26.60.04 (arm64-v8a) на glibc/AArch64

Отдельная от 32-битного порта (0.16 / 1.1.5 / 1.2) ветка работ. Новая сборка **64-битная** (AArch64, hard-float), поэтому
здесь свой загрузчик ELF64 и своя Bionic-прослойка; код из `src/` (ARMv7) не переиспользуется как есть.

Файлы игры в репозиторий **не кладутся** (репозиторий публичный). Нужны `lib/arm64-v8a/*.so` из APK.

## Статус: этап 1 — загрузка библиотек (проверено под qemu-aarch64, на R36S НЕ запускалось)
| Что | Результат |
|---|---|
| Маппинг `libminecraftpe.so` (326 МБ) | файл-бэкенд `mmap` (MAP_PRIVATE), в ОЗУ не копируется |
| Релокации (`RELATIVE/ABS64/GLOB_DAT/JUMP_SLOT/IRELATIVE`) | 1 098 637 в `libminecraftpe.so`, 0 неподдерживаемых |
| Библиотеки | libc++_shared, libfmod, libHttpClient.Android, libPlayFabMultiplayer, libmaesdk, libminecraftpe |
| Неразрешённые (strong) символы | 0 |
| Статические конструкторы `init_array` | 2373 в `libminecraftpe.so` отработали без падения (qemu) |

Сборка и проверка на ПК:
```
sudo apt install gcc-aarch64-linux-gnu qemu-user libc6-arm64-cross
make -C port64 CROSS=aarch64-linux-gnu-
qemu-aarch64 -L /usr/aarch64-linux-gnu port64/build/loadtest <папка с lib/arm64-v8a> [--init]
```
На самой R36S (если userland aarch64): `make -C port64 && port64/build/loadtest <libdir> --init`.
Вывод `loadtest` (stdout): список `UNRESOLVED` (были бы заглушки-ловушки) и `WEAK-UNRESOLVED` (остаются NULL).

## Что осталось (этап 2+), по результату `WEAK-UNRESOLVED` для `libminecraftpe.so`
Все импорты игры слабые (weak), поэтому нереализованное остаётся `NULL` и падает только при вызове:
* **82 × `gl*` + 16 × `egl*`** — EGL поверх SDL2 и GLES (на arm64 без soft-float обёрток, проще, чем в `src/libegl.c`);
* **33 × Android NDK** (`AAssetManager_*`, `AAsset_*`, `AAssetDir_*`, `ALooper_*`, `ANativeWindow_*`, `AConfiguration_*`,
  `APerformanceHint_*`) — свой `libandroid.so`; ассеты (567 МБ в `assets/assets`) читать из обычных файлов;
* **JNI**: игра на `GameActivity` (androidgamesdk), 95 экспортов `Java_*` (MainActivity, XAL/Xbox Live, FMOD, Swappy)
  и обратные вызовы в Java через `JNIEnv` — нужна фейковая JVM (см. `src/jni_fake.c`), но методов намного больше;
* **Play Integrity / PairIP**: в APK есть `libpairipcore.so`, у игры есть `nativeSetIntegrityToken` — нужно понять,
  блокирует ли это старт без Google Play;
* Bionic-раскладки, не покрытые прослойкой: `setjmp/longjmp` (jmp_buf), `dlopen/dlsym` из игры, `pthread_gettid_np` для чужих потоков;
* Память: R36S ≈ 1 ГБ ОЗУ — главный риск; проверить реальный RSS после загрузки и в мире.

Раскладки структур (`pthread_attr_t` 56 Б, `sigaction`, `addrinfo`, `sysconf`-константы, `_ctype_`, `__sF`) написаны по
заголовкам Bionic и не проверены на устройстве.
