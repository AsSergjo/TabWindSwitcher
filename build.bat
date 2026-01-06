@echo off
setlocal

REM Очистка предыдущих сборок
del *.obj 2>nul
del TabWindSwitcher.exe 2>nul
del resources.res 2>nul REM Добавлена очистка для файла ресурсов

REM Попытка настроить окружение для 64-битной компиляции
echo Setting up 64-bit environment...
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" 2>nul
if errorlevel 1 (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" 2>nul
    if errorlevel 1 (
        call "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvars64.bat" 2>nul
        if errorlevel 1 (
            echo ERROR: Cannot setup 64-bit Visual Studio environment
            echo Please run from "x64 Native Tools Command Prompt for VS"
            pause
            exit /b 1
        )
    )
)

echo Compiling resources...
rc.exe /fo resources.res resources.rc
if errorlevel 1 (
    echo RESOURCE COMPILATION FAILED
    pause
    exit /b 1
)

echo Compiling 64-bit application...

cl.exe /nologo /W4 /EHsc /O2 /DUNICODE /D_UNICODE /D_WIN64 /std:c++17 TabWindSwitcher.cpp resources.res user32.lib gdi32.lib gdiplus.lib ole32.lib psapi.lib shlwapi.lib /Fe:TabWindSwitcher.exe

if errorlevel 1 (
    echo COMPILATION FAILED
    pause
    exit /b 1
)

echo BUILD SUCCESSFUL!
echo Verifying architecture...
dumpbin /headers TabWindSwitcher.exe | findstr "machine"
if errorlevel 1 (
    echo WARNING: Cannot verify architecture
)

pause