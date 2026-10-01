@echo off
set "CONFIG=Release"
set "BIN_DIR=_bin\Filter_x64"

if /I "%~1"=="Debug" (
    set "CONFIG=Debug"
    set "BIN_DIR=_bin\Filter_x64_Debug"
)

echo Copying %CONFIG% MpcVideoRenderer64.ax to C:\Program Files\MPC-BE x64\...
copy /Y "%BIN_DIR%\MpcVideoRenderer64.ax" "C:\Program Files\MPC-BE x64\MpcVideoRenderer64.ax"
if %ERRORLEVEL% equ 0 (
    echo.
    echo [SUCCESS] MpcVideoRenderer64.ax successfully deployed to MPC-BE directory!
    echo Launching MPC-BE...
    start "" "C:\Program Files\MPC-BE x64\mpc-be64.exe"
    echo.
    echo Press any key to kill MPC-BE...
    pause
    taskkill /F /IM mpc-be64.exe /T >nul 2>&1
) else (
    echo.
    echo [PERMISSION ERROR] Administrator privileges required.
    echo Right-click deploy_to_mpc.cmd and select "Run as administrator".
    echo.
    pause
)
