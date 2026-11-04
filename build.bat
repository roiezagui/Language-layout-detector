@echo off
rem Build LayoutGuard.exe with MSVC (Visual Studio "Developer Command Prompt") or MinGW-w64.
rem The result is a single self-contained exe (no runtime install needed).

where cl >nul 2>nul
if %errorlevel%==0 (
    cl /nologo /O2 /MT /DUNICODE /D_UNICODE LayoutGuard.c /link /SUBSYSTEM:WINDOWS user32.lib shell32.lib ole32.lib /OUT:LayoutGuard.exe
    goto :end
)

where gcc >nul 2>nul
if %errorlevel%==0 (
    gcc -O2 -s -static -mwindows -municode LayoutGuard.c -o LayoutGuard.exe -luser32 -lshell32 -lole32
    goto :end
)

echo No compiler found. Install Visual Studio Build Tools (cl) or MinGW-w64 (gcc).
exit /b 1

:end
if exist LayoutGuard.exe echo Built LayoutGuard.exe
del /q LayoutGuard.obj 2>nul
