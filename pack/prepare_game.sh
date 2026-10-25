#!/bin/sh
# Usage: prepare_game.sh minecraft-0-16-0.apk <package-dir>
# Extracts the (legally owned) APK next to the launcher. Nothing from the APK is redistributed.
set -e
APK="$1"; OUT="$2"
[ -f "$APK" ] && [ -n "$OUT" ] || { echo "usage: $0 game.apk outdir"; exit 1; }
TMP=$(mktemp -d)
unzip -q -o "$APK" -d "$TMP"
mkdir -p "$OUT/libs" "$OUT/assets"
cp "$TMP/lib/armeabi-v7a/libminecraftpe.so" "$TMP/lib/armeabi-v7a/libfmod.so" "$OUT/libs/"
cp -r "$TMP/assets/." "$OUT/assets/"
rm -rf "$TMP"
echo "game files ready in $OUT"
