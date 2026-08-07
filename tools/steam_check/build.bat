@echo off
setlocal enabledelayedexpansion

rem Builds and runs the discovery check against the real Steam client.
rem
rem Separate from build.bat because it produces a console program rather than the mod,
rem and because it should be runnable on its own in a second: the rules it checks are the
rem ones whose failures only ever showed up as a two player test going wrong.

set ROOT=%~dp0..\..
set OUT=%ROOT%\build

where cl.exe >nul 2>&1
if errorlevel 1 (
    set VCVARS=
    for %%E in (Community Professional Enterprise BuildTools) do (
        if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" (
            if not defined VCVARS set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat"
        )
    )
    if not defined VCVARS (
        echo ERROR: could not find vcvars64.bat.
        exit /b 1
    )
    call "!VCVARS!" >nul
)

if not exist "%OUT%\steam_check" mkdir "%OUT%\steam_check"

cl /nologo /std:c++20 /EHsc /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /MT ^
   /DNOMINMAX /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /DMPE_VERSION_STRING=\"0.0.0\" ^
   /I "%ROOT%\src" /Fo"%OUT%\steam_check\\" /Fe"%OUT%\steam_check.exe" ^
   "%~dp0steam_check.cpp" ^
   "%ROOT%\src\Net\PacketProtocol.cpp" ^
   "%ROOT%\src\Lobby\Discovery.cpp" ^
   "%ROOT%\src\Net\SteamSocketsTransport.cpp" ^
   "%ROOT%\src\Lobby\SteamMatchmakingHooks.cpp" ^
   "%ROOT%\src\Steam\SteamApi.cpp" ^
   "%ROOT%\src\Core\GameBuild.cpp" ^
   "%ROOT%\src\Net\IPeerTransport.cpp" ^
   "%ROOT%\src\Core\Result.cpp" ^
   "%ROOT%\src\Core\Log.cpp" ^
   "%ROOT%\src\Core\Hash.cpp"
if errorlevel 1 (
    echo BUILD FAILED: steam_check
    exit /b 1
)

rem The Steam client attaches a process to a game by this file, so it has to sit beside the
rem executable. Without it SteamAPI_Init refuses and the check reports that rather than a
rem failure, which is correct: nothing was proven either way.
echo 2806050> "%OUT%\steam_appid.txt"

rem Run from the output directory, because the Steam client looks for steam_appid.txt in
rem the working directory rather than beside the executable.
set GAME_BINARIES=%ROOT%\..\Meteorite\Binaries\Win64
cd /d "%OUT%"
"%OUT%\steam_check.exe" "%GAME_BINARIES%"
exit /b %ERRORLEVEL%
