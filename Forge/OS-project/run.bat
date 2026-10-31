@echo off
setlocal
cd /d "%~dp0"

where wsl >nul 2>nul
if errorlevel 1 (
  echo WSL is not installed or is not available.
  echo This project uses Linux APIs and must run in Ubuntu/WSL.
  echo Install Ubuntu with: wsl --install -d Ubuntu
  exit /b 1
)

echo Launching the Linux supervisor through Ubuntu/WSL...
wsl.exe -d Ubuntu -- bash -c "chmod +x run.sh && ./run.sh"
exit /b %errorlevel%
