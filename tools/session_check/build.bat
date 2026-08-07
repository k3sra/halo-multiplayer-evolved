@echo off
setlocal enabledelayedexpansion

rem Builds and runs a host and a client against each other in one process.
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

if not exist "%OUT%\session_check" mkdir "%OUT%\session_check"

cl /nologo /std:c++20 /EHsc /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /MT ^
   /DNOMINMAX /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /DMPE_VERSION_STRING=\"0.0.0\" ^
   /I "%ROOT%\src" /Fo"%OUT%\session_check\\" /Fe"%OUT%\session_check.exe" ^
   "%~dp0session_check.cpp" ^
   "%ROOT%\src\Net\PacketProtocol.cpp" ^
   "%ROOT%\src\Lobby\LobbyManager.cpp" ^
   "%ROOT%\src\Engine\IEngineControl.cpp" ^
   "%ROOT%\src\Map\MapVariant.cpp" ^
   "%ROOT%\src\Core\Json.cpp" ^
   "%ROOT%\src\Core\GameBuild.cpp" ^
   "%ROOT%\src\Map\MapVariantParser.cpp" ^
   "%ROOT%\src\Net\IPeerTransport.cpp" ^
   "%ROOT%\src\Core\Result.cpp" ^
   "%ROOT%\src\Core\Log.cpp" ^
   "%ROOT%\src\Core\Hash.cpp"
if errorlevel 1 (
    echo BUILD FAILED: session_check
    exit /b 1
)

"%OUT%\session_check.exe"
exit /b %ERRORLEVEL%
