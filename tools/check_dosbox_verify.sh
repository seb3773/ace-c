#!/bin/sh
# Host-side analysis after the DOSBox run.
#  * summarizes DOS RESULTS.TXT
#  * forward : our archives must decode with our ace
#  * reverse : DOSMADE.ACE (authored by the REAL DOS ACE) must decode with
#              our ace -- the cross-compatibility proof
# (diagnostic script: intentionally no `set -e`, we want every section run)
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
VDIR="$ROOT/tools/dosbox-verify"
cd "$ROOT"

echo "===== DOSBox RESULTS.TXT (markers) ====="
if [ -f "$VDIR/RESULTS.TXT" ]; then
    grep -aiE 'rc=|====|error|fail|bad|password|cannot' "$VDIR/RESULTS.TXT" \
        || echo "(no markers found - full log below)"
    echo "--- full log ---"
    cat "$VDIR/RESULTS.TXT"
else
    echo "(missing: run 'sh tools/dosbox-run-verify.sh' inside DOSBox first)"
fi

echo
echo "===== FORWARD: our *.ACE decode with our ace ====="
for a in "$VDIR"/*.ACE; do
    case "$(basename "$a")" in
        DOSMADE.ACE) continue ;;
        PW.ACE) opts="-p SECRET" ;;
        *) opts="" ;;
    esac
    if ./ace t $opts "$a" >/dev/null 2>&1; then
        printf 'ok ace     %s\n' "$(basename "$a")"
    else
        printf 'FAIL ace   %s\n' "$(basename "$a")"
    fi
done

echo
echo "===== REVERSE: DOSMADE.ACE (real DOS ACE) -> our ace ====="
if [ -f "$VDIR/DOSMADE.ACE" ]; then
    ./ace l "$VDIR/DOSMADE.ACE" || true
    if ./ace t "$VDIR/DOSMADE.ACE"; then
        rm -rf "$VDIR/_out"
        ./ace x -d "$VDIR/_out" "$VDIR/DOSMADE.ACE"
        echo "extracted files:"
        ls -l "$VDIR/_out"
    else
        echo "FAIL: our ace could not test the DOS-authored archive"
    fi
else
    echo "(no DOSMADE.ACE yet - it is produced by the DOSBox VERIFY.BAT run)"
fi
