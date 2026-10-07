ACE DOSBox cross-verification directory
==========================================

This folder was filled by tools/gen_dosbox_verify.sh. It holds:
  *.ACE        archives produced by OUR Linux compressor (mkace/ace)
  ACE.EXE      the real DOS WinACE 2.6 decoder/compressor
  VERIFY.BAT   runs ACE.EXE over every archive -> RESULTS.TXT

Run it inside an interactive DOSBox:
  mount c <paste-the-full-path-of-this-folder>
  c:
  VERIFY.BAT

Progress shows on screen ([verify] X.ACE...); all ACE output lands in
RESULTS.TXT. If it ever stalls, the last screen line names the culprit.

Then, back on the host:
  sh tools/check_dosbox_verify.sh
  cat tools/dosbox-verify/RESULTS.TXT

PW.ACE is encrypted (password SECRET); it is intentionally NOT in the
batch because DOS ACE would prompt and stall a non-interactive run.
