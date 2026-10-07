#!/bin/sh
# Historical Wine wrapper. ace26.exe is an ACE-SFX (DOS PMODE/W stub),
# not a Win32 PE. Use tools/dosbox-ace.sh instead:
#
#   ./ace x -d testdata/ace26sfx research/binary_win32/ace26.exe
#   tools/dosbox-ace.sh a -y -std -s- -m3 -c2 OUT.ACE FILE.TXT
#
# ACE32.EXE inside the SFX is Win32/UPX. Wine (amd64) on Debian 12 has
# no wine32 candidate; DOS ACE.EXE under DOSBox is the working path.
echo "ace26.exe is DOS SFX, not Win32 PE. Use tools/dosbox-ace.sh" >&2
exit 1
