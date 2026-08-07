@echo off
setlocal enabledelayedexpansion

rem MultiplayerEvolved build.
rem
rem Uses cl.exe directly rather than CMake, because a Visual Studio install always
rem has the compiler while CMake is a separate download. The project has no third
rem party dependencies, so a direct invocation is all that is needed.
rem
rem   build.bat            release build
rem   build.bat debug      debug build with symbols and no optimization
rem   build.bat install    release build, then copy into the game
rem   build.bat package    release build, then make the release zip in build\
rem
rem Output lands in build\.

set CONFIG=release
set DO_INSTALL=0
set DO_PACKAGE=0
if /I "%~1"=="debug"   set CONFIG=debug
if /I "%~1"=="install" set DO_INSTALL=1
if /I "%~1"=="package" set DO_PACKAGE=1

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
set COMMON=/nologo /std:c++20 /EHsc /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /MT /DNOMINMAX /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /DMPE_VERSION_STRING=\"0.1.0\" /I"%ROOT%src"

if "%CONFIG%"=="debug" (
    set CFLAGS=%COMMON% /Od /Zi /D_DEBUG /Fd"%OUT%\MultiplayerEvolved.pdb"
    set LFLAGS=/DEBUG
) else (
    set CFLAGS=%COMMON% /O2 /Zi /DNDEBUG /Fd"%OUT%\MultiplayerEvolved.pdb"
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
 "%ROOT%src\Lobby\Discovery.cpp"^
 "%ROOT%src\Lobby\LobbyManager.cpp"^
 "%ROOT%src\Map\MapVariant.cpp"^
 "%ROOT%src\Map\MapVariantParser.cpp"^
 "%ROOT%src\Map\MapVariantInjector.cpp"^
 "%ROOT%src\Update\UpdateCheck.cpp"

echo.
echo === Building MultiplayerEvolved.dll (%CONFIG%) ===
rem user32.lib is needed for the window based startup gate, which waits for the game to
rem finish loading before any memory scanning begins.
cl %CFLAGS% /LD %SOURCES% /Fo"%OUT%\\" /Fe"%OUT%\MultiplayerEvolved.dll" /link %LFLAGS% version.lib user32.lib
if errorlevel 1 (
    echo.
    echo BUILD FAILED: MultiplayerEvolved.dll
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
rem
rem Written flat, with a goto rather than one large parenthesised block.
rem
rem The block this replaced ended each copy with "|| exit /b 1" and looked correct.
rem It was not: inside a parenthesised if, that abandoned the rest of the install but
rem still left the script exiting zero, so a copy that failed reported success to
rem whatever ran the build. That shipped a release built from one version while the
rem game folder still held the previous one.
if not "%DO_INSTALL%"=="1" goto :package

if not exist "%INSTALL_DIR%\HaloCampaignEvolved.exe" (
    echo.
    echo ERROR: %INSTALL_DIR% does not look like the game directory.
    exit /b 1
)

rem Say who is holding the file, before touching anything.
rem
rem The game keeps both DLLs mapped while it runs and Windows will not let a mapped
rem image be replaced, so a copy over them fails with nothing more useful than
rem "Access is denied." Naming the cause up front is the difference between closing
rem the game and going looking for a permissions problem that does not exist.
tasklist /FI "IMAGENAME eq HaloCampaignEvolved.exe" /NH 2>nul | find /I "HaloCampaignEvolved.exe" >nul
if not errorlevel 1 (
    echo.
    echo ERROR: the game is running and holds MultiplayerEvolved.dll. Close it first.
    echo        Nothing was copied. The game folder still has the previous build.
    exit /b 1
)

echo.
echo === Installing into %INSTALL_DIR% ===

copy /Y "%OUT%\MultiplayerEvolved.dll" "%INSTALL_DIR%\" >nul
if errorlevel 1 (
    echo.
    echo INSTALL FAILED: could not copy MultiplayerEvolved.dll into %INSTALL_DIR%.
    echo                 The game folder still has the previous build.
    exit /b 1
)

copy /Y "%OUT%\version.dll" "%INSTALL_DIR%\" >nul
if errorlevel 1 (
    echo.
    echo INSTALL FAILED: could not copy version.dll into %INSTALL_DIR%.
    echo                 MultiplayerEvolved.dll was replaced and version.dll was not,
    echo                 so the two no longer match. Close the game and build again.
    exit /b 1
)

if not exist "%INSTALL_DIR%\MultiplayerEvolved" mkdir "%INSTALL_DIR%\MultiplayerEvolved"
xcopy /Y /E /I "%ROOT%data" "%INSTALL_DIR%\MultiplayerEvolved" >nul
if errorlevel 1 (
    echo.
    echo INSTALL FAILED: could not copy the data folder into %INSTALL_DIR%\MultiplayerEvolved.
    exit /b 1
)

rem What landed, rather than what was meant to.
rem
rem A copy that quietly did nothing and a copy that worked read the same from here.
rem The size and the time tell them apart at a glance, which is the check that was
rem missing when a stale build went out.
echo.
echo Installed into %INSTALL_DIR%
for %%F in ("%INSTALL_DIR%\MultiplayerEvolved.dll" "%INSTALL_DIR%\version.dll") do (
    echo   %%~nxF  %%~zF bytes  %%~tF
)

rem --- Package -----------------------------------------------------------------
rem
rem The release archive is built here rather than by hand.
rem
rem Hand assembling it shipped several releases whose zip held the two DLLs and
rem nothing else. The MultiplayerEvolved folder was created empty and then dropped
rem by the archiver, so the data folder never went in, and the install instructions
rem told players to copy three things when only two existed. Anyone installing
rem fresh got no symbol descriptor.
rem
rem Everything the install needs comes from one place now, and the contents are
rem listed afterwards so an empty or missing folder is visible rather than assumed.
:package
if not "%DO_PACKAGE%"=="1" goto :finished

set STAGE=%OUT%\package
if exist "%STAGE%" rmdir /S /Q "%STAGE%"
mkdir "%STAGE%\MultiplayerEvolved"

copy /Y "%OUT%\MultiplayerEvolved.dll" "%STAGE%\" >nul
if errorlevel 1 (
    echo PACKAGE FAILED: MultiplayerEvolved.dll is missing from %OUT%.
    exit /b 1
)
copy /Y "%OUT%\version.dll" "%STAGE%\" >nul
if errorlevel 1 (
    echo PACKAGE FAILED: version.dll is missing from %OUT%.
    exit /b 1
)
xcopy /Y /E /I "%ROOT%data" "%STAGE%\MultiplayerEvolved" >nul
if errorlevel 1 (
    echo PACKAGE FAILED: the data folder could not be staged.
    exit /b 1
)

rem The symbol descriptor is the one file whose absence is not obvious until the
rem engine binding quietly falls back to its built in defaults, so it is checked
rem by name rather than trusted to have come along with the rest.
dir /b /s "%STAGE%\MultiplayerEvolved\symbols\*.json" >nul 2>&1
if errorlevel 1 (
    echo PACKAGE FAILED: no symbol descriptor was staged; the archive would install
    echo                 a mod that falls back to built in defaults.
    exit /b 1
)

set ARCHIVE=%OUT%\MultiplayerEvolved.zip
if exist "%ARCHIVE%" del /Q "%ARCHIVE%"
powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%\*' -DestinationPath '%ARCHIVE%' -Force"
if errorlevel 1 (
    echo PACKAGE FAILED: the archive could not be written.
    exit /b 1
)

echo.
echo === Packaged %ARCHIVE% ===
powershell -NoProfile -Command "Add-Type -AssemblyName System.IO.Compression.FileSystem; $z=[System.IO.Compression.ZipFile]::OpenRead('%ARCHIVE%'); foreach ($e in $z.Entries) { '  {0}  {1} bytes' -f $e.FullName, $e.Length }; $z.Dispose()"

:finished
endlocal






