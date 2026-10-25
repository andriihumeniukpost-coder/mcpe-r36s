#!/bin/bash
# PortMaster-style launcher for ArkOS (R36S). Place the folder `mcpe/` next to this script.
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
if [ -d "/opt/system/Tools/PortMaster/" ]; then controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then controlfolder="$XDG_DATA_HOME/PortMaster"
else controlfolder="/roms/ports/PortMaster"; fi
[ -f "$controlfolder/control.txt" ] && source "$controlfolder/control.txt"

GAMEDIR="$(cd "$(dirname "$0")/mcpe" && pwd)"
cd "$GAMEDIR"
exec > "$GAMEDIR/log.txt" 2>&1

# Android-style names for the system libraries (32-bit armhf)
LIBSYS=/usr/lib/arm-linux-gnueabihf
[ -d "$LIBSYS" ] || LIBSYS=/usr/lib32
ln -sf "$LIBSYS/libGLESv2.so.2"  libs/libGLESv2.so
ln -sf "$LIBSYS/libz.so.1"       libs/libz.so
ln -sf "$LIBSYS/libstdc++.so.6"  libs/libstdc++.so

export LD_LIBRARY_PATH="$GAMEDIR/libs:$LIBSYS:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG_FILE="$controlfolder/gamecontrollerdb.txt"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

$GPTOKEYB "mcpe" &   # optional, ignored if absent
./mcpe
kill %1 2>/dev/null
