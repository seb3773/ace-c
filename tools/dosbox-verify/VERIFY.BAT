@ECHO OFF
REM Cross-compat check: our ACE archives must LIST+TEST under the real
REM DOS ACE.EXE (WinACE 2.6). Results land in RESULTS.TXT.
SET RES=RESULTS.TXT
ECHO ===== ACE 2.6 DOS verification =====> %RES%
ECHO Host-authored archives are decoded here.>> %RES%
ECHO [verify] STORE.ACE...
ECHO.>> %RES%
ECHO ===== STORE.ACE =====>> %RES%
ACE.EXE L -y -std STORE.ACE>> %RES%
ECHO L_rc=%ERRORLEVEL%>> %RES%
ACE.EXE T -y -std STORE.ACE>> %RES%
ECHO T_rc=%ERRORLEVEL%>> %RES%
ECHO [verify] LZ77.ACE...
ECHO.>> %RES%
ECHO ===== LZ77.ACE =====>> %RES%
ACE.EXE L -y -std LZ77.ACE>> %RES%
ECHO L_rc=%ERRORLEVEL%>> %RES%
ACE.EXE T -y -std LZ77.ACE>> %RES%
ECHO T_rc=%ERRORLEVEL%>> %RES%
ECHO [verify] BLOCKED.ACE...
ECHO.>> %RES%
ECHO ===== BLOCKED.ACE =====>> %RES%
ACE.EXE L -y -std BLOCKED.ACE>> %RES%
ECHO L_rc=%ERRORLEVEL%>> %RES%
ACE.EXE T -y -std BLOCKED.ACE>> %RES%
ECHO T_rc=%ERRORLEVEL%>> %RES%
ECHO [verify] SOLID.ACE...
ECHO.>> %RES%
ECHO ===== SOLID.ACE =====>> %RES%
ACE.EXE L -y -std SOLID.ACE>> %RES%
ECHO L_rc=%ERRORLEVEL%>> %RES%
ACE.EXE T -y -std SOLID.ACE>> %RES%
ECHO T_rc=%ERRORLEVEL%>> %RES%
ECHO [verify] DELTA.ACE...
ECHO.>> %RES%
ECHO ===== DELTA.ACE =====>> %RES%
ACE.EXE L -y -std DELTA.ACE>> %RES%
ECHO L_rc=%ERRORLEVEL%>> %RES%
ACE.EXE T -y -std DELTA.ACE>> %RES%
ECHO T_rc=%ERRORLEVEL%>> %RES%
ECHO [verify] EXE.ACE...
ECHO.>> %RES%
ECHO ===== EXE.ACE =====>> %RES%
ACE.EXE L -y -std EXE.ACE>> %RES%
ECHO L_rc=%ERRORLEVEL%>> %RES%
ACE.EXE T -y -std EXE.ACE>> %RES%
ECHO T_rc=%ERRORLEVEL%>> %RES%
ECHO [verify] PIC.ACE...
ECHO.>> %RES%
ECHO ===== PIC.ACE =====>> %RES%
ACE.EXE L -y -std PIC.ACE>> %RES%
ECHO L_rc=%ERRORLEVEL%>> %RES%
ACE.EXE T -y -std PIC.ACE>> %RES%
ECHO T_rc=%ERRORLEVEL%>> %RES%
ECHO [verify] CM.ACE...
ECHO.>> %RES%
ECHO ===== CM.ACE =====>> %RES%
ACE.EXE L -y -std CM.ACE>> %RES%
ECHO L_rc=%ERRORLEVEL%>> %RES%
ACE.EXE T -y -std CM.ACE>> %RES%
ECHO T_rc=%ERRORLEVEL%>> %RES%
ECHO [verify] creating DOSMADE.ACE from CHECK.TXT...
ECHO.>> %RES%
ECHO ===== create an archive from within DOS =====>> %RES%
ECHO Authored by DOS ACE under DOSBox.> CHECK.TXT
ECHO Extra line for compressible content.>> CHECK.TXT
ACE.EXE A -y -std -s- -m3 DOSMADE.ACE CHECK.TXT>> %RES%
ACE.EXE T -y -std DOSMADE.ACE>> %RES%
ECHO DOSMADE_T_rc=%ERRORLEVEL%>> %RES%
ECHO.>> %RES%
ECHO Done. On the host run: sh tools/check_dosbox_verify.sh>> %RES%
ECHO (it decodes DOSMADE.ACE with our ace as reverse check)>> %RES%
ECHO Verification complete.>> %RES%
