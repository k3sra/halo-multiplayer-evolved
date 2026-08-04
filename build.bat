@echo off
setlocal enabledelayedexpansion

rem ForgeEvolved build.
rem
rem Uses cl.exe directly rather than CMake, because a Visual Studio install always
rem has the compiler while CMake is a separate download. The project has no third
rem party dependencies, so a direct invocation is all that is needed.
rem
rem   build.bat            release build
rem   build.bat debug      debug build with symbols and no optimization
rem   build.bat install    release build, then copy into the game
rem
rem Output lands in build\.

set CONFIG=release
set DO_INSTALL=0
if /I "%~1"=="debug"   set CONFIG=debug
if /I "%~1"=="install" set DO_INSTALL=1

set ROOT=%~dp0
set OUT=%ROOT%build
set GAME_DIR=%ROOT%..
set INSTALL_DIR=%GAME_DIR%\Meteorite\Binaries\Win64

rem --- Locate the compiler ---------------------------------------------------
where cl.exe >nul 2>&1
if errorlevel 1 (
    set VCVARS=
    for %%E in (Community Professional Enterprise BuildTools) do (
        if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" (
            if not defined VCVARS set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat"
        )
    )
    if not defined VCVARS (
        echo ERROR: could not find vcvars64.bat. Install Visual Studio 2022 with the
        echo        "Desktop development with C++" workload.
        exit /b 1
    )
    echo Using !VCVARS!
    call "!VCVARS!" >nul
    if errorlevel 1 (
        echo ERROR: vcvars64.bat failed.
        exit /b 1
    )
)

if not exist "%OUT%" mkdir "%OUT%"

rem --- Flags ----------------------------------------------------------------
rem /MT rather than /MD: static CRT means the mod has no Visual C++
rem redistributable dependency, which removes the single most common install
rem failure for a non technical user.
set COMMON=/nologo /std:c++20 /EHsc /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /MT /DNOMINMAX /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /DFE_VERSION_STRING=\"0.1.0\" /I"%ROOT%src"

if "%CONFIG%"=="debug" (
    set CFLAGS=%COMMON% /Od /Zi /D_DEBUG /Fd"%OUT%\ForgeEvolved.pdb"
    set LFLAGS=/DEBUG
) else (
    set CFLAGS=%COMMON% /O2 /Zi /DNDEBUG /Fd"%OUT%\ForgeEvolved.pdb"
    set LFLAGS=/DEBUG /OPT:REF /OPT:ICF
)

rem --- Sources --------------------------------------------------------------
set SOURCES=^
 "%ROOT%src\ModMain.cpp"^
 "%ROOT%src\Core\Result.cpp"^
 "%ROOT%src\Core\Log.cpp"^
 "%ROOT%src\Core\Hash.cpp"^
 "%ROOT%src\Core\Json.cpp"^
 "%ROOT%src\Core\GameBuild.cpp"^
 "%ROOT%src\Core\Pacing.cpp"^
 "%ROOT%src\Debug\AccessTrap.cpp"^
 "%ROOT%src\Steam\SteamApi.cpp"^
 "%ROOT%src\Blam\ModuleImage.cpp"^
 "%ROOT%src\Blam\PatternScanner.cpp"^
 "%ROOT%src\Blam\SymbolRegistry.cpp"^
 "%ROOT%src\Blam\DebugGlobals.cpp"^
 "%ROOT%src\Unreal\NamePool.cpp"^
 "%ROOT%src\Unreal\ProcessMemory.cpp"^
 "%ROOT%src\Unreal\FNameTrampoline.cpp"^
 "%ROOT%src\Unreal\GameThread.cpp"^
 "%ROOT%src\Unreal\LobbyUI.cpp"^
 "%ROOT%src\Unreal\ObjectArray.cpp"^
 "%ROOT%src\Unreal\Reflection.cpp"^
 "%ROOT%src\Engine\IEngineControl.cpp"^
 "%ROOT%src\Net\IPeerTransport.cpp"^
 "%ROOT%src\Net\PacketProtocol.cpp"^
 "%ROOT%src\Net\SteamSocketsTransport.cpp"^
 "%ROOT%src\Lobby\SteamMatchmakingHooks.cpp"^
 "%ROOT%src\Lobby\LobbyManager.cpp"^
 "%ROOT%src\Map\MapVariant.cpp"^
 "%ROOT%src\Map\MapVariantParser.cpp"^
 "%ROOT%src\Map\MapVariantInjector.cpp"^
 "%ROOT%src\Update\UpdateCheck.cpp"

echo.
echo === Building ForgeEvolved.dll (%CONFIG%) ===
rem user32.lib is needed for the window based startup gate, which waits for the game to
rem finish loading before any memory scanning begins.
cl %CFLAGS% /LD %SOURCES% /Fo"%OUT%\\" /Fe"%OUT%\ForgeEvolved.dll" /link %LFLAGS% version.lib user32.lib
if errorlevel 1 (
    echo.
    echo BUILD FAILED: ForgeEvolved.dll
    exit /b 1
)

echo.
echo === Building version.dll (loader proxy) ===
cl %CFLAGS% /LD "%ROOT%loader\version_proxy.cpp" /Fo"%OUT%\loader_" /Fe"%OUT%\version.dll" /link %LFLAGS%
if errorlevel 1 (
    echo.
    echo BUILD FAILED: version.dll
    exit /b 1
)

echo.
echo === Build succeeded ===
dir /b "%OUT%\*.dll"

rem --- Install --------------------------------------------------------------
if "%DO_INSTALL%"=="1" (
    if not exist "%INSTALL_DIR%\HaloCampaignEvolved.exe" (
        echo.
        echo ERROR: %INSTALL_DIR% does not look like the game directory.
        exit /b 1
    )
    echo.
    echo === Installing into %INSTALL_DIR% ===
    copy /Y "%OUT%\ForgeEvolved.dll" "%INSTALL_DIR%\" >nul || exit /b 1
    copy /Y "%OUT%\version.dll"      "%INSTALL_DIR%\" >nul || exit /b 1
    if not exist "%INSTALL_DIR%\ForgeEvolved" mkdir "%INSTALL_DIR%\ForgeEvolved"
    xcopy /Y /E /I "%ROOT%data" "%INSTALL_DIR%\ForgeEvolved" >nul || exit /b 1
    echo Installed.
)

endlocal






