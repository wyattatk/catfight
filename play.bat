@echo off
REM catfight -- launch the built game
REM
REM The build output lives in build-catfight\Release, but the game assets are
REM authored here in the source tree. Radiant's gamepack points its "run engine"
REM button at this script, and it is the convenient way to launch by hand too.
REM
REM   play.bat              start the game
REM   play.bat cf_test      start the game and load a map
REM
REM Assets are copied into the build output by the catfight_assets CMake target,
REM which runs as part of a normal build. If a texture or map edit does not show
REM up in game, that copy has not run:
REM
REM   cmake --build build-catfight --target catfight_assets

setlocal
set "ROOT=%~dp0"
set "REL=%ROOT%build-catfight\Release"

if not exist "%REL%\catfight.exe" (
    echo.
    echo   catfight.exe not found at %REL%
    echo   Build it first:
    echo     cmake -S . -B build-catfight -G Ninja -DCMAKE_BUILD_TYPE=Release ^
-DBUILD_STANDALONE=ON -DBUILD_GAME_QVMS=OFF -DBUILD_MISSIONPACK=OFF
    echo     cmake --build build-catfight
    echo.
    exit /b 1
)

if "%~1"=="" (
    start "" "%REL%\catfight.exe" +set fs_basepath "%REL%"
) else (
    start "" "%REL%\catfight.exe" +set fs_basepath "%REL%" +map %1
)
endlocal
