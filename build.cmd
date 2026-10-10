@echo off
rem Builds ClaudeDictate.exe. Uses whichever compiler is available:
rem   1. MSVC (cl), if already on PATH (Developer Command Prompt)
rem   2. LLVM clang, if on PATH (needs the Windows SDK from VS / Build Tools)
rem   3. MSVC found automatically through vswhere (no developer prompt needed)
setlocal
cd /d "%~dp0"

set LIBS=user32.lib gdi32.lib gdiplus.lib dwmapi.lib advapi32.lib ole32.lib oleaut32.lib shell32.lib

where cl >nul 2>nul && goto msvc

where clang >nul 2>nul && (
    echo Building with clang...
    llvm-rc /nologo /fo claude_dictate.res claude_dictate.rc || exit /b 1
    clang -O2 -Wall -Wextra claude_dictate.c claude_dictate.res -o ClaudeDictate.exe ^
        -luser32 -lgdi32 -lgdiplus -ldwmapi -ladvapi32 -lole32 -loleaut32 -lshell32 ^
        -Wl,/subsystem:windows || exit /b 1
    goto done
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto nocompiler
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto nocompiler
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || goto nocompiler

:msvc
echo Building with MSVC...
rc /nologo /fo claude_dictate.res claude_dictate.rc || exit /b 1
cl /nologo /O2 /W3 claude_dictate.c claude_dictate.res /Fe:ClaudeDictate.exe ^
    /link /SUBSYSTEM:WINDOWS %LIBS% || exit /b 1
del /q claude_dictate.obj 2>nul
goto done

:nocompiler
echo No C compiler found. Install one of:
echo   - Visual Studio Build Tools with "Desktop development with C++":
echo       winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
echo   - LLVM (also needs the Build Tools above for the Windows SDK):
echo       winget install LLVM.LLVM
exit /b 1

:done
echo Built ClaudeDictate.exe
