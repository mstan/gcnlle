@echo off
REM gcnrecomp oracle M5b: boot the GC IPL WITH the M5 dummy disc inserted
REM (--ipl-disc, the new local CLI patch in MainNoGUI.cpp) and capture the
REM disc-interaction trace (DI-block MMIO, 0xCC006000 range). Same determinism
REM knobs as dolphin_trace_ext.bat: interpreter CPU, single core, Null video,
REM No-Audio, fixed RTC/region/language/scan. GCN_TRACE_MAX_SC=0 runs through
REM every `sc` (BS2 handlers + whatever the IPL does servicing the disc read),
REM bounded only by GCN_TRACE_MAX_INSNS. NO_RETIRED keeps the file compact
REM (MMIO/EXI/DMA/INTR/GX/VI/DSP events only) for the value+order diff.
REM The dummy disc has a ZERO apploader/DOL/FST offset table on purpose (see
REM tools/make_dummy_disc.py) — the IPL hitting that null apploader IS the
REM expected reference behavior this capture exists to record, not a bug.
set "PATH=C:\Windows\System32;C:\Windows;C:\Windows\System32\Wbem"
set "GCN_TRACE_OUT=F:\Projects\gcnrecomp\oracle\traces\dolphin_disc_dummy.trace"
set "GCN_TRACE_MAX_SC=0"
set "GCN_TRACE_NO_RETIRED=1"
if "%GCN_TRACE_MAX_INSNS%"=="" set "GCN_TRACE_MAX_INSNS=5000000"
set "DISC=%~2"
if "%DISC%"=="" set "DISC=F:\Projects\gcnrecomp\_work\dummy.iso"
set "EXE=%~1"
if "%EXE%"=="" set "EXE=F:\Projects\gcnrecomp\oracle\dolphin\Binary\x64\DolphinNoGUI.exe"
cd /d "F:\Projects\gcnrecomp\oracle\dolphin"
echo RUN START %DATE% %TIME% > "F:\Projects\gcnrecomp\oracle\run_trace_disc.log"
"%EXE%" --platform=headless --boot-gc-ipl=NTSC_U --ipl-disc="%DISC%" -v Null -C Dolphin.Core.CPUCore=0 -C Dolphin.Core.CPUThread=False -C Dolphin.Core.EnableCustomRTC=True -C Dolphin.Core.CustomRTCValue=1072915200 -C Dolphin.Core.GFXBackend=Null -C Dolphin.Core.SelectedLanguage=0 -C Dolphin.Core.ProgressiveScan=False -C Dolphin.Interface.UsePanicHandlers=False -C Dolphin.DSP.Backend="No Audio Output" -C Dolphin.Core.DSPHLE=True --user=F:\Projects\gcnrecomp\oracle\dolphin-user >> "F:\Projects\gcnrecomp\oracle\run_trace_disc.log" 2>&1
echo DOLPHIN_EXIT=%errorlevel% >> "F:\Projects\gcnrecomp\oracle\run_trace_disc.log"
echo RUN END %DATE% %TIME% >> "F:\Projects\gcnrecomp\oracle\run_trace_disc.log"
