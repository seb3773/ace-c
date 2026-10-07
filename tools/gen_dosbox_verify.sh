#!/bin/sh
# Build a self-contained DOSBox verification directory: our compressor's ACE
# archives + a batch that the real DOS ACE.EXE (WinACE 2.6) runs to prove the
# encoder/decoder cross-compatibility. Run this on the host, then mount the
# produced directory in DOSBox and execute VERIFY.BAT (see README.TXT inside).
#
#   sh tools/gen_dosbox_verify.sh
#
# Overridable: ACE_DOS=/path/to/ACE.EXE
set -e
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
VDIR="$ROOT/tools/dosbox-verify"
ACE_DOS="${ACE_DOS:-$ROOT/testdata/ace26sfx/ACE.EXE}"

[ -x "$ROOT/ace" ] || make -C "$ROOT" all >/dev/null
if [ ! -f "$ACE_DOS" ]; then
    echo "missing DOS ACE.EXE at $ACE_DOS" >&2
    echo "extract it first:  ./ace x -d testdata/ace26sfx research/binary_win32/ace26.exe" >&2
    exit 1
fi

rm -rf "$VDIR"
mkdir -p "$VDIR"
cd "$VDIR"
cp "$ACE_DOS" ACE.EXE

# --- deterministic sample inputs (DOS 8.3 uppercase names) ---
python3 - <<'PY'
import os
def w(name, data):
    with open(name, "wb") as f:
        f.write(data)

w("HELLO.TXT", (b"Hello from the Linux ACE compressor!\n" * 24))
w("NOTES.TXT", (b"Line of notes. " * 200 + b"\n"))
# smooth ramp -> DELTA-friendly
w("DATA.BIN", bytes([(i * 3) & 0xFF for i in range(4096)]))
# MZ + code-like -> EXE preprocessing
w("PROBE.BIN", b"MZ" + bytes(range(256)) * 4 + b"\xE8\x10\x00\x90" * 32)
# tiled 3-byte pixels -> PIC-style
w("PIXELS.BIN", bytes([ (i % 3 == 0) and 0xFF or (i & 0x7F) for i in range(3072)]))
PY

M="$ROOT/ace"

# --- archives produced by OUR compressor ---
"$M" a STORE.ACE   -0 HELLO.TXT
"$M" a LZ77.ACE    -z HELLO.TXT NOTES.TXT
"$M" a BLOCKED.ACE -2 HELLO.TXT NOTES.TXT DATA.BIN
"$M" a SOLID.ACE   -s -z HELLO.TXT NOTES.TXT
"$M" a DELTA.ACE   -2 -dl DATA.BIN
"$M" a EXE.ACE     -2 -xe PROBE.BIN
"$M" a PIC.ACE     -2 -p 3 PIXELS.BIN
"$M" a CM.ACE      -2 -cm "MAIN COMMENT FROM LINUX ace" -cf "FILE COMMENT FROM LINUX" \
     NOTES.TXT

# optional encrypted sample (NOT auto-tested in the batch: DOS ACE would
# prompt for the password and stall a non-interactive run).
"$M" a PW.ACE      -z -pw SECRET HELLO.TXT

ls -1 *.ACE > ARCLIST.TXT

# --- VERIFY.BAT : run inside DOSBox (explicit lines, CRLF endings) ---
# The DOSBox bundled shell does not reliably expand FOR %%A loops nor
# SETLOCAL delayed-expansion (!VAR!), so we emit one explicit block per
# archive and read the exit code via the standard %ERRORLEVEL%.
{
  echo '@ECHO OFF'
  echo 'REM Cross-compat check: our ACE archives must LIST+TEST under the real'
  echo 'REM DOS ACE.EXE (WinACE 2.6). Results land in RESULTS.TXT.'
  echo 'SET RES=RESULTS.TXT'
  echo 'ECHO ===== ACE 2.6 DOS verification =====> %RES%'
  echo 'ECHO Host-authored archives are decoded here.>> %RES%'
  for A in STORE LZ77 BLOCKED SOLID DELTA EXE PIC CM; do
    echo 'ECHO [verify] '"$A"'.ACE...'
    echo 'ECHO.>> %RES%'
    echo 'ECHO ===== '"$A"'.ACE =====>> %RES%'
    echo 'ACE.EXE L -y -std '"$A"'.ACE>> %RES%'
    echo 'ECHO L_rc=%ERRORLEVEL%>> %RES%'
    echo 'ACE.EXE T -y -std '"$A"'.ACE>> %RES%'
    echo 'ECHO T_rc=%ERRORLEVEL%>> %RES%'
  done
  echo 'ECHO [verify] creating DOSMADE.ACE from CHECK.TXT...'
  echo 'ECHO.>> %RES%'
  echo 'ECHO ===== create an archive from within DOS =====>> %RES%'
  echo 'ECHO Authored by DOS ACE under DOSBox.> CHECK.TXT'
  echo 'ECHO Extra line for compressible content.>> CHECK.TXT'
  echo 'ACE.EXE A -y -std -s- -m3 DOSMADE.ACE CHECK.TXT>> %RES%'
  echo 'ACE.EXE T -y -std DOSMADE.ACE>> %RES%'
  echo 'ECHO DOSMADE_T_rc=%ERRORLEVEL%>> %RES%'
  echo 'ECHO.>> %RES%'
  echo 'ECHO Done. On the host run: sh tools/check_dosbox_verify.sh>> %RES%'
  echo 'ECHO (it decodes DOSMADE.ACE with our ace as reverse check)>> %RES%'
  echo 'ECHO Verification complete.>> %RES%'
} | sed 's/$/\r/' > VERIFY.BAT

# --- README.TXT (CRLF) : how to mount and run ---
printf '%s\r\n' \
 'ACE DOSBox cross-verification directory' \
 '==========================================' \
 '' \
 'This folder was filled by tools/gen_dosbox_verify.sh. It holds:' \
 '  *.ACE        archives produced by OUR Linux compressor (ace)' \
 '  ACE.EXE      the real DOS WinACE 2.6 decoder/compressor' \
 '  VERIFY.BAT   runs ACE.EXE over every archive -> RESULTS.TXT' \
 '' \
 'Run it inside an interactive DOSBox:' \
 '  mount c <paste-the-full-path-of-this-folder>' \
 '  c:' \
 '  VERIFY.BAT' \
 '' \
 'Progress shows on screen ([verify] X.ACE...); all ACE output lands in' \
 'RESULTS.TXT. If it ever stalls, the last screen line names the culprit.' \
 '' \
 'Then, back on the host:' \
 '  sh tools/check_dosbox_verify.sh' \
 '  cat tools/dosbox-verify/RESULTS.TXT' \
 '' \
 'PW.ACE is encrypted (password SECRET); it is intentionally NOT in the' \
 'batch because DOS ACE would prompt and stall a non-interactive run.' \
 > README.TXT

echo "generated in $VDIR:"
ls -1 *.ACE VERIFY.BAT README.TXT
