@echo off
REM gcnrecomp oracle: boot the real GC IPL headless + emit a trace_format.h stream.
REM Determinism: interpreter CPU, single core, Null video, No-Audio, CustomRTC
REM 1072915200 (2004-01-01 -> GC RTC 126230400), English/stereo/NTSC-U/non-progressive.
REM Requires the IPL at  <user>\GC\USA\IPL.bin  (see oracle\dolphin-user).
REM Arg1 = path to DolphinNoGUI.exe. Env GCN_TRACE_MAX_INSNS caps the run
REM (the tap also stops at the first `sc`).
set "PATH=C:\Windows\System32;C:\Windows;C:\Windows\System32\Wbem"
set "GCN_TRACE_OUT=F:\Projects\gcnrecomp\oracle\traces\dolphin_ipl_usa.trace"
if "%GCN_TRACE_MAX_INSNS%"=="" set "GCN_TRACE_MAX_INSNS=300000"
set "EXE=%~1"
if "%EXE%"=="" set "EXE=F:\Projects\gcnrecomp\oracle\dolphin\Binary\x64\DolphinNoGUI.exe"
cd /d "F:\Projects\gcnrecomp\oracle\dolphin"
echo RUN START %DATE% %TIME% > "F:\Projects\gcnrecomp\oracle\run_trace.log"
"%EXE%" --platform=headless --boot-gc-ipl=NTSC_U -v Null -C Dolphin.Core.CPUCore=0 -C Dolphin.Core.CPUThread=False -C Dolphin.Core.EnableCustomRTC=True -C Dolphin.Core.CustomRTCValue=1072915200 -C Dolphin.Core.GFXBackend=Null -C Dolphin.Core.SelectedLanguage=0 -C Dolphin.Core.ProgressiveScan=False -C Dolphin.Interface.UsePanicHandlers=False -C Dolphin.DSP.Backend="No Audio Output" -C Dolphin.Core.DSPHLE=True --user=F:\Projects\gcnrecomp\oracle\dolphin-user >> "F:\Projects\gcnrecomp\oracle\run_trace.log" 2>&1
echo DOLPHIN_EXIT=%errorlevel% >> "F:\Projects\gcnrecomp\oracle\run_trace.log"
echo RUN END %DATE% %TIME% >> "F:\Projects\gcnrecomp\oracle\run_trace.log"
type "F:\Projects\gcnrecomp\oracle\run_trace.log"
