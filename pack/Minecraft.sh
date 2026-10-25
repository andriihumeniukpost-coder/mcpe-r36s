#!/bin/bash
# PortMaster-style launcher for ArkOS (R36S). Folder `mcpe/` must sit next to this script.
HERE="$(cd "$(dirname "$0")" && pwd)"
GAMEDIR="$HERE/mcpe"
LOG="$GAMEDIR/log.txt"
mkdir -p "$GAMEDIR" 2>/dev/null
# everything below is logged, even early failures
exec > "$LOG" 2>&1
echo "== launcher $(date)"
echo "== uname: $(uname -a)"
echo "== script dir: $HERE"
fail() { echo "FATAL: $*"; sleep 8; exit 1; }

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
for d in /opt/system/Tools/PortMaster /opt/tools/PortMaster "$XDG_DATA_HOME/PortMaster" /roms/ports/PortMaster; do
  [ -d "$d" ] && { controlfolder="$d"; break; }
done
echo "== PortMaster: ${controlfolder:-none}"
[ -f "$controlfolder/control.txt" ] && source "$controlfolder/control.txt"

cd "$GAMEDIR" || fail "no $GAMEDIR"
chmod +x ./mcpe 2>/dev/null          # zip extraction on iPad/Windows drops the exec bit
[ -f ./mcpe ] || fail "mcpe binary missing in $GAMEDIR"
echo "== files:"; ls -la "$GAMEDIR" "$GAMEDIR/libs" 2>&1 | head -40

# 32-bit ARM support present?
[ -e /lib/ld-linux-armhf.so.3 ] || [ -e /lib32/ld-linux-armhf.so.3 ] || [ -e /usr/lib/arm-linux-gnueabihf/ld-linux-armhf.so.3 ] \
  || echo "WARNING: no armhf dynamic loader found - 32-bit binaries may not run on this system"

# First run: unpack game files from the APK placed next to the launcher
if [ ! -f libs/libminecraftpe.so ]; then
  APK=$(ls "$GAMEDIR"/*.apk 2>/dev/null | head -1)
  [ -n "$APK" ] || fail "put minecraft-0-16-0.apk into $GAMEDIR"
  echo "== extracting $APK"
  if command -v unzip >/dev/null; then
    unzip -o -j "$APK" "lib/armeabi-v7a/libminecraftpe.so" "lib/armeabi-v7a/libfmod.so" -d libs || fail "unzip libs failed"
    unzip -o -q "$APK" "assets/*" -d "$GAMEDIR" || fail "unzip assets failed"
  elif command -v python3 >/dev/null; then
    python3 - "$APK" "$GAMEDIR" <<'PY' || fail "python unzip failed"
import sys, zipfile, os
apk, out = sys.argv[1:3]
z = zipfile.ZipFile(apk)
os.makedirs(out + "/libs", exist_ok=True)
for n in z.namelist():
    if n in ("lib/armeabi-v7a/libminecraftpe.so", "lib/armeabi-v7a/libfmod.so"):
        open(out + "/libs/" + os.path.basename(n), "wb").write(z.read(n))
    elif n.startswith("assets/") and not n.endswith("/"):
        p = os.path.join(out, n); os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "wb").write(z.read(n))
PY
  else
    fail "neither unzip nor python3 available - extract the APK on a PC (see README)"
  fi
fi
[ -f libs/libminecraftpe.so ] || fail "libs/libminecraftpe.so missing"

LIBSYS=""
for d in /usr/lib/arm-linux-gnueabihf /usr/lib32 /lib/arm-linux-gnueabihf; do [ -d "$d" ] && { LIBSYS="$d"; break; }; done
echo "== 32-bit lib dir: ${LIBSYS:-NOT FOUND}"

export LD_LIBRARY_PATH="$GAMEDIR/libs:$LIBSYS:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG_FILE="$controlfolder/gamecontrollerdb.txt"
[ -n "$sdl_controllerconfig" ] && export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export LD_DEBUG_OUTPUT=
echo "== ldd mcpe:"; ldd ./mcpe 2>&1 | head -30

[ -n "$GPTOKEYB" ] && { $GPTOKEYB "mcpe" & }
echo "== starting"
./mcpe
RC=$?
echo "== mcpe exited with code $RC"
[ -n "$GPTOKEYB" ] && kill %1 2>/dev/null
# keep the message on screen briefly instead of flashing away
[ $RC -ne 0 ] && sleep 5
exit $RC
