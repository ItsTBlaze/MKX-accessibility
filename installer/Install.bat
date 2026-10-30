@echo off
setlocal EnableExtensions EnableDelayedExpansion
title MKX Access installer
cd /d "%~dp0"
echo MKX Access installer
echo.

if /I "%~1"=="/uninstall" goto :uninstall

for %%F in (MKXAccess.asi nvdaControllerClient64.dll LICENSE-NVDA.txt dinput8.dll) do (
    if not exist "%%F" (
        echo The file %%F is missing.
        echo Please unzip the whole MKXAccess folder first, then run Install again from the unzipped folder.
        goto :fail
    )
)

call :checkrunning || goto :fail
echo Looking for Mortal Kombat X...
call :findgame
if not defined GAME (
    echo Could not find Mortal Kombat X. It needs the Steam version installed.
    echo.
    echo To install by hand: in Steam, open the context menu on Mortal Kombat X, choose Manage, then Browse local files.
    echo Open Binaries, then Retail, and copy dinput8.dll, MKXAccess.asi, nvdaControllerClient64.dll and LICENSE-NVDA.txt there.
    goto :fail
)
echo Found the game in !GAME!

copy /Y "MKXAccess.asi" "!GAME!\" >nul || goto :copyfail
copy /Y "nvdaControllerClient64.dll" "!GAME!\" >nul || goto :copyfail
copy /Y "LICENSE-NVDA.txt" "!GAME!\" >nul || goto :copyfail

rem Keep another mod's dinput8.dll (such as MKXHook's loader), which also loads MKX Access.
set "KEPT="
for %%A in ("dinput8.dll") do set "OURSIZE=%%~zA"
if exist "!GAME!\dinput8.dll" (
    for %%A in ("!GAME!\dinput8.dll") do set "THEIRSIZE=%%~zA"
    if "!THEIRSIZE!"=="!OURSIZE!" (set "REPLACE=1") else if "!THEIRSIZE!"=="4608" (set "REPLACE=1") else (set "REPLACE=")
) else (
    set "REPLACE=1"
)
if defined REPLACE (
    copy /Y "dinput8.dll" "!GAME!\" >nul || goto :copyfail
) else (
    set "KEPT=1"
)

echo.
echo MKX Access is installed.
echo Start NVDA, then start Mortal Kombat X. About 2 seconds after the game window appears you will hear MKX Access and its version number.
if defined KEPT echo You already had another dinput8.dll, probably from MKXHook. It was kept, and it loads MKX Access too.
goto :done

:uninstall
call :checkrunning || goto :fail
call :findgame
if not defined GAME (
    echo Could not find Mortal Kombat X, so there is nothing to remove.
    goto :fail
)
for %%F in (MKXAccess.asi nvdaControllerClient64.dll LICENSE-NVDA.txt MKXAccess.log MKXAccess-README.txt) do (
    if exist "!GAME!\%%F" del /F /Q "!GAME!\%%F"
)
rem Only remove our own small loader, never another mod's.
if exist "!GAME!\dinput8.dll" (
    for %%A in ("!GAME!\dinput8.dll") do set "THEIRSIZE=%%~zA"
    set "OURSIZE=0"
    if exist "dinput8.dll" for %%A in ("dinput8.dll") do set "OURSIZE=%%~zA"
    if "!THEIRSIZE!"=="!OURSIZE!" del /F /Q "!GAME!\dinput8.dll"
    if "!THEIRSIZE!"=="4608" del /F /Q "!GAME!\dinput8.dll"
)
echo MKX Access has been removed. The game is back to normal.
goto :done

:checkrunning
for %%P in (MK10.exe MK10Game.exe MKXLauncher.exe) do (
    tasklist /FI "IMAGENAME eq %%P" 2>nul | find /I "%%P" >nul
    if not errorlevel 1 (
        echo Mortal Kombat X is running. Please close the game, then run this again.
        exit /b 1
    )
)
exit /b 0

:findgame
set "GAME="
rem 1. Steam's own install folder, from the registry
for %%K in ("HKCU\Software\Valve\Steam|SteamPath" "HKLM\SOFTWARE\WOW6432Node\Valve\Steam|InstallPath" "HKLM\SOFTWARE\Valve\Steam|InstallPath") do (
    for /f "tokens=1,2 delims=|" %%a in (%%K) do (
        for /f "tokens=2,*" %%A in ('reg query "%%a" /v %%b 2^>nul ^| find /I "%%b"') do (
            set "STEAM=%%B"
            set "STEAM=!STEAM:/=\!"
            call :trylib "!STEAM!"
            rem 2. every library folder Steam knows about
            if exist "!STEAM!\steamapps\libraryfolders.vdf" (
                for /f "usebackq tokens=1,*" %%X in ("!STEAM!\steamapps\libraryfolders.vdf") do (
                    if /I "%%~X"=="path" (
                        set "LIB=%%~Y"
                        set "LIB=!LIB:\\=\!"
                        call :trylib "!LIB!"
                    )
                )
            )
        )
    )
)
rem 3. common places, in case the registry is missing
for %%D in (C D E F G) do (
    if exist "%%D:\" (
        for %%P in ("SteamLibrary" "Steam" "Program Files (x86)\Steam" "Program Files\Steam" "Games\Steam" "Games\SteamLibrary") do call :trylib "%%D:\%%~P"
    )
)
exit /b 0

:trylib
if defined GAME exit /b 0
if exist "%~1\steamapps\common\MK10\Binaries\Retail\MK10.exe" set "GAME=%~1\steamapps\common\MK10\Binaries\Retail"
exit /b 0

:copyfail
echo.
echo Could not copy the files into the game folder.
echo Try running Install again. If it keeps failing, open the context menu on Install and choose Run as administrator.
goto :fail

:fail
echo.
pause
exit /b 1

:done
echo.
pause
exit /b 0
