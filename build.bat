@echo off
rem ============================================================
rem  FileSearchTool build script
rem  Toolchain: D:\soft\tools\mingw64 (MinGW-W64 GCC)
rem  Output:    FileSearchTool.exe (static, Win7/10/11 x64)
rem ============================================================
setlocal
set "MINGW=D:\soft\tools\mingw64"
set "PATH=%MINGW%\bin;%PATH%"

where gcc >nul 2>nul
if errorlevel 1 (
    echo [ERROR] gcc not found under: %MINGW%
    exit /b 1
)

set "CFLAGS=-O2 -Wall -Wextra -municode -DUNICODE -D_UNICODE -D__USE_MINGW_ANSI_STDIO=1 -DWINVER=0x0601 -D_WIN32_WINNT=0x0601"

if not exist build mkdir build

pushd res
windres.exe app.rc -O coff -o ..\build\app.res
if errorlevel 1 goto :err
popd

for %%S in (util results search ui main selftest) do (
    echo [CC] %%S.c
    gcc %CFLAGS% -c src\%%S.c -o build\%%S.o
    if errorlevel 1 goto :err
)

echo [LINK] FileSearchTool.exe
gcc -municode -mwindows -static -o FileSearchTool.exe ^
    build\main.o build\ui.o build\search.o build\results.o build\util.o build\selftest.o ^
    build\app.res ^
    -lcomctl32 -lshell32 -lole32 -lgdi32 -luser32
if errorlevel 1 goto :err

echo [CHECK] DLL dependencies:
objdump -p FileSearchTool.exe | findstr "DLL Name"

echo.
echo [OK] build finished: %~dp0FileSearchTool.exe
goto :eof

:err
popd 2>nul
echo [ERROR] build failed
exit /b 1
