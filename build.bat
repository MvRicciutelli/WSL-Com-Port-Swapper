@echo off
setlocal

set OUT=WslComPortSwapper.exe

:: ── try MSVC (cl.exe) ──────────────────────────────────────────────────────
where cl >nul 2>&1
if %errorlevel% == 0 (
    echo Building with MSVC...
    mt.exe -nologo -manifest app.manifest -outputresource:main.c.res;1 >nul 2>&1
    cl /nologo /O2 /W3 main.c /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:app.manifest user32.lib advapi32.lib comctl32.lib /out:%OUT%
    goto done
)

:: ── try MinGW gcc ──────────────────────────────────────────────────────────
where gcc >nul 2>&1
if %errorlevel% == 0 (
    echo Building with gcc...
    windres manifest.rc -o manifest_res.o
    gcc -O2 -Wall main.c manifest_res.o -o %OUT% -luser32 -ladvapi32 -lcomctl32 -mwindows
    goto done
)

echo.
echo ERROR: No C compiler found.
echo.
echo Options:
echo   1. MSVC  — run this script from a "Developer Command Prompt for VS"
echo   2. MinGW — install from https://www.mingw-w64.org/ and add bin\ to PATH
echo.
exit /b 1

:done
if exist %OUT% (
    echo.
    echo Built: %OUT%
    echo Run as Administrator for attach/detach to work.
)
