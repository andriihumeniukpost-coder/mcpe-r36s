#!/bin/bash
# PortMaster-style launcher for ArkOS (R36S). Folder `mcpe/` must sit next to this script.
HERE="$(cd "$(dirname "$0")" && pwd)"
GAMEDIR="$HERE/mcpe"
LOG="$GAMEDIR/log.txt"
mkdir -p "$GAMEDIR" 2>/dev/null
# everything below is logged, even early failures
exec > "$LOG" 2>&1
# flush the log to the SD card every second, so a freeze/reboot still leaves the trail
( while true; do sync; sleep 1; done ) &
SYNCPID=$!
echo "== launcher $(date)"
echo "== uname: $(uname -a)"
echo "== script dir: $HERE"
fail() { echo "FATAL: $*"; sync; sleep 8; kill $SYNCPID 2>/dev/null; exit 1; }

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

# Unpack game files from the APK placed next to the launcher (again whenever the APK is replaced: 0.8.1 <-> 0.16.0)
APK=$(ls -t "$GAMEDIR"/*.apk 2>/dev/null | head -1)
APKID=""
[ -n "$APK" ] && APKID="$(basename "$APK") $(stat -c %s "$APK" 2>/dev/null)"
if [ ! -f libs/libminecraftpe.so ] || { [ -n "$APK" ] && [ "$(cat libs/.apk 2>/dev/null)" != "$APKID" ]; }; then
  [ -n "$APK" ] || fail "put the Minecraft PE apk (0.8.1 or 0.16.0) into $GAMEDIR"
  echo "== extracting $APK"
  rm -f libs/libminecraftpe.so libs/libfmod.so libs/.apk; rm -rf "$GAMEDIR/assets"; mkdir -p libs
  if command -v unzip >/dev/null; then
    unzip -o -j "$APK" "lib/armeabi-v7a/libminecraftpe.so" -d libs || fail "unzip libs failed"
    unzip -o -j "$APK" "lib/armeabi-v7a/libfmod.so" -d libs >/dev/null 2>&1   # absent in old builds (0.8.x): that is fine
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
  printf '%s' "$APKID" > libs/.apk
fi
if [ -f libs/libfmod.so ]; then echo "== game flavour: modern (libfmod present)"; else echo "== game flavour: legacy 0.8.x (no libfmod)"; fi
[ -f libs/libminecraftpe.so ] || fail "libs/libminecraftpe.so missing"

LIBSYS=""
for d in /usr/lib/arm-linux-gnueabihf /usr/lib32 /lib/arm-linux-gnueabihf; do [ -d "$d" ] && { LIBSYS="$d"; break; }; done
echo "== 32-bit lib dir: ${LIBSYS:-NOT FOUND}"

export LD_LIBRARY_PATH="$GAMEDIR/libs:$LIBSYS:$LD_LIBRARY_PATH"
# safe mode: create an empty file mcpe/nopipe to turn off the pipelined presenter (if the screen stays black)
[ -f "$GAMEDIR/nopipe" ] && export MCPE_PIPE=0 && echo "== pipelined presenter disabled (nopipe file)"
export SDL_GAMECONTROLLERCONFIG_FILE="$controlfolder/gamecontrollerdb.txt"
[ -n "$sdl_controllerconfig" ] && export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
rm -f "$GAMEDIR"/.[0-9]* "$GAMEDIR"/ldtrace* 2>/dev/null   # leftovers of earlier LD_DEBUG runs
echo "== deps of mcpe (armhf loader):"; LD_TRACE_LOADED_OBJECTS=1 ./mcpe 2>&1 | head -30
echo "== 32-bit runtime libs:"; ls -la "$LIBSYS"/libSDL2* "$LIBSYS"/libstdc++* "$LIBSYS"/libgcc_s* "$LIBSYS"/libEGL* "$LIBSYS"/libGLESv2* 2>&1 | head -20

[ -n "$GPTOKEYB" ] && { $GPTOKEYB "mcpe" & }
# performance: pin CPU and GPU governors to "performance" while the game runs (restored afterwards)
SAVED=""
setgov() { echo "$2" | sudo -n tee "$1" >/dev/null 2>&1 || echo "$2" > "$1" 2>/dev/null; }
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor /sys/class/devfreq/*/governor; do
  [ -e "$g" ] || continue
  SAVED="$SAVED$g=$(cat "$g" 2>/dev/null)
"
  setgov "$g" performance
done
echo "== governors now: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>&1) / gpu: $(cat /sys/class/devfreq/*/governor 2>&1 | tr '\n' ' ')"
echo "== starting"; sync
# diagnostics: free memory + game RSS every second, so a silent kill leaves a trail (look for [mem] lines)
( while kill -0 $$ 2>/dev/null; do
    P=$(pidof mcpe 2>/dev/null | awk '{print $1}')
    echo "[mem] $(date +%T) avail=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo)MB free=$(awk '/MemFree/{print int($2/1024)}' /proc/meminfo)MB swapfree=$(awk '/SwapFree/{print int($2/1024)}' /proc/meminfo)MB rss=$(awk '/VmRSS/{print int($2/1024)}' /proc/${P:-0}/status 2>/dev/null)MB"
    sleep 1
  done ) &
MEMPID=$!
# diagnostics: per-core CPU load (%) once per second -> [cpu] lines (core0 core1 core2 core3 ...)
( prev=$(grep '^cpu[0-9]' /proc/stat)
  while kill -0 $$ 2>/dev/null; do
    sleep 1; cur=$(grep '^cpu[0-9]' /proc/stat)
    echo "[cpu] $(date +%T) $(printf '%s\n%s\n' "$prev" "$cur" | awk '{ if (seen[$1]++) { t=0; for(i=2;i<=NF;i++) t+=$i; idle=$5+$6; dt=t-T[$1]; di=idle-I[$1]; printf "%d ", (dt>0) ? 100*(dt-di)/dt : 0 } else { t=0; for(i=2;i<=NF;i++) t+=$i; T[$1]=t; I[$1]=$5+$6 } }')"
    prev="$cur"
  done ) &
CPUPID=$!
T0=$(date +%s)
./mcpe
RC=$?
echo "== mcpe ran for $(( $(date +%s) - T0 )) s"
kill $MEMPID $CPUPID 2>/dev/null
echo "== kernel log (oom/kill/lowmem), if readable:"; { sudo -n dmesg 2>/dev/null || dmesg 2>/dev/null; } | grep -i -E "oom|out of memory|killed process|lowmem|mali|segfault" | tail -8
printf '%s' "$SAVED" | while IFS== read -r g v; do [ -n "$g" ] && [ -n "$v" ] && setgov "$g" "$v"; done
echo "== mcpe exited with code $RC"
if [ $RC -eq 1 ]; then
  echo "== load failed; re-running with dynamic-loader trace"
  LD_DEBUG=files,libs LD_DEBUG_OUTPUT="$GAMEDIR/ldtrace" ./mcpe >/dev/null 2>&1
  for f in "$GAMEDIR"/ldtrace*; do
    echo "---- $f (last 60 lines)"
    tail -60 "$f"
    echo "---- trace from first libfmod mention (60 lines):"
    sed -n '/libfmod.so/,$p' "$f" | grep -v -E "^ *[0-9]+:\s*$" | head -60
  done
fi
[ -n "$GPTOKEYB" ] && kill %1 2>/dev/null
# keep the message on screen briefly instead of flashing away
sync
[ $RC -ne 0 ] && sleep 5
kill $SYNCPID 2>/dev/null
exit $RC
