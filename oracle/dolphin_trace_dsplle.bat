@echo off
REM gcnrecomp oracle: EXTENDED trace with the REAL DSP (LLE interpreter) instead
REM of HLE. Requires dsp_rom.bin + dsp_coef.bin in <user>\GC\. DSPHLE=False +
REM DSP.EnableJIT=False selects the deterministic LLE interpreter.
set "PATH=C:\Windows\System32;C:\Windows;C:\Windows\System32\Wbem"
set "GCN_TRACE_OUT=F:\Projects\gcnrecomp\oracle\traces\dolphin_ipl_usa_dsplle.trace"
set "GCN_TRACE_MAX_SC=0"
set "GCN_TRACE_NO_RETIRED=1"
if "%GCN_TRACE_MAX_INSNS%"=="" set "GCN_TRACE_MAX_INSNS=5000000"
set "EXE=%~1"
if "%EXE%"=="" set "EXE=F:\Projects\gcnrecomp\oracle\dolphin\Binary\x64\DolphinNoGUI.exe"
cd /d "F:\Projects\gcnrecomp\oracle\dolphin"
echo RUN START %DATE% %TIME% > "F:\Projects\gcnrecomp\oracle\run_trace_dsplle.log"
"%EXE%" --platform=headless --boot-gc-ipl=NTSC_U -v Null -C Dolphin.Core.CPUCore=0 -C Dolphin.Core.CPUThread=False -C Dolphin.Core.EnableCustomRTC=True -C Dolphin.Core.CustomRTCValue=1072915200 -C Dolphin.Core.GFXBackend=Null -C Dolphin.Core.SelectedLanguage=0 -C Dolphin.Core.ProgressiveScan=False -C Dolphin.Interface.UsePanicHandlers=False -C Dolphin.DSP.Backend="No Audio Output" -C Dolphin.Core.DSPHLE=False -C Dolphin.DSP.EnableJIT=False >> "F:\Projects\gcnrecomp\oracle\run_trace_dsplle.log" 2>&1
echo DOLPHIN_EXIT=%errorlevel% >> "F:\Projects\gcnrecomp\oracle\run_trace_dsplle.log"
echo RUN END %DATE% %TIME% >> "F:\Projects\gcnrecomp\oracle\run_trace_dsplle.log"
