#!/bin/sh
# Run commandline ACE 2.6 (DOS ACE.EXE) under DOSBox.
#
# research/binary_win32/ace26.exe is an ACE-SFX, not a PE. Extract it first:
#
#   ./ace x -d testdata/ace26sfx research/binary_win32/ace26.exe
#
# DOSBox is the right host: ACE.EXE is a 32-bit DOS binary (PMODE/W),
# not Win32. Wine cannot load it. ACE32.EXE is Win32/UPX but needs Wine.
#
# Usage:
#   tools/dosbox-ace.sh a -y -std -s- -m3 -c2 OUT.ACE FILE.TXT
#   tools/dosbox-ace.sh l -y -std OUT.ACE
set -e
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
ACE="$ROOT/testdata/ace26sfx/ACE.EXE"
if [ ! -f "$ACE" ]; then
    echo "missing $ACE" >&2
    echo "extract with: ./ace x -d testdata/ace26sfx research/binary_win32/ace26.exe" >&2
    exit 1
fi
if ! command -v dosbox >/dev/null 2>&1; then
    echo "dosbox not found" >&2
    exit 1
fi
WORKDIR="${ACE_DOS_WORKDIR:-/tmp/dosace}"
mkdir -p "$WORKDIR"
cp "$ACE" "$WORKDIR/ACE.EXE"
CONF="$WORKDIR/dosbox.conf"
{
    printf '%s\n' '[sdl]' 'fullscreen=false' 'output=surface'
    printf '%s\n' '[mixer]' 'nosound=true'
    printf '%s\n' '[midi]' 'mpu401=none'
    printf '%s\n' '[sblaster]' 'sbtype=none'
    printf '%s\n' '[speaker]' 'pcspeaker=false'
    printf '%s\n' '[cpu]' 'core=auto' 'cycles=max'
    printf '%s\n' '[dosbox]' 'memsize=16'
    printf '%s\n' '[autoexec]'
    echo "mount c $WORKDIR"
    echo "c:"
    printf 'ACE.EXE'
    for a in "$@"; do
        printf ' %s' "$a"
    done
    echo
    echo exit
} > "$CONF"
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}"
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"
exec dosbox -conf "$CONF" -noconsole -exit
