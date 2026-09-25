@echo off
cd /d "%~dp0"
title Boiler FVE — graf
echo Spoustim graf. Toto okno NEZAVirejte.
echo Prohlizec: http://127.0.0.1:8080/
echo.
where python >nul 2>&1
if errorlevel 1 (
  echo Python neni v PATH. Otevri nove okno PowerShell a zkus: python --version
  pause
  exit /b 1
)
python -c "import serial" >nul 2>&1
if errorlevel 1 (
  echo Instaluji pyserial...
  python -m pip install -r requirements.txt
  if errorlevel 1 (
    echo pip install selhal. Zkus rucne: python -m pip install -r requirements.txt
    pause
    exit /b 1
  )
)
if "%~1"=="" (
  python web\server.py
) else (
  python web\server.py --port %~1
)
echo.
echo Server skoncil.
pause
