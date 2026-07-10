@echo off
REM gcnrecomp oracle: build the patched local Dolphin (DolphinNoGUI, Release x64).
REM Uses a CLEAN PATH so the msys2/devkitPro cmake does not shadow the VS cmake
REM (glslang's external build needs the "Visual Studio 17" generator).
set "PATH=C:\Windows\System32;C:\Windows;C:\Windows\System32\Wbem;C:\Windows\System32\WindowsPowerShell\v1.0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cd /d "F:\Projects\gcnrecomp\oracle\dolphin"
echo BUILD START %DATE% %TIME% > "F:\Projects\gcnrecomp\oracle\build_dolphin.log"
REM Build the vcxproj directly (its ProjectReferences pull in DolphinLib + all
REM externals). msbuild /t:DolphinNoGUI on the .sln misfires on this tree.
msbuild Source\Core\DolphinNoGUI\DolphinNoGUI.vcxproj /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo >> "F:\Projects\gcnrecomp\oracle\build_dolphin.log" 2>&1
echo MSBUILD_EXIT=%errorlevel% >> "F:\Projects\gcnrecomp\oracle\build_dolphin.log"
echo BUILD END %DATE% %TIME% >> "F:\Projects\gcnrecomp\oracle\build_dolphin.log"
type "F:\Projects\gcnrecomp\oracle\build_dolphin.log" | findstr /C:"MSBUILD_EXIT"
