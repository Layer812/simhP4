@echo off
setlocal
cd /d "%~dp0"

echo ============================================================
echo simhP4 S0 SELECTOR - MACHINE ^> OS
echo Branch: microvax-bsd43
echo KEEP PDP-7 Release Final path / NO CLEAN / COM9 app-flash
echo ============================================================

for /f "delims=" %%B in ('git rev-parse --abbrev-ref HEAD 2^>nul') do set "BRANCH=%%B"
if /i not "%BRANCH%"=="microvax-bsd43" (
  echo ERROR: current branch is "%BRANCH%"
  echo Please run:
  echo   git fetch origin
  echo   git checkout microvax-bsd43
  exit /b 2
)

if not exist "G:\esp-idf-5.5.4\export.bat" (
  echo ERROR: G:\esp-idf-5.5.4\export.bat not found
  exit /b 3
)

call G:\esp-idf-5.5.4\export.bat
if errorlevel 1 (
  echo ERROR: ESP-IDF environment activation failed
  exit /b 4
)

echo.
echo [1/3] BUILD - no clean
idf.py build
if errorlevel 1 (
  echo.
  echo BUILD FAILED. Nothing was flashed.
  exit /b 5
)

echo.
echo [2/3] APP-FLASH COM9
idf.py -p COM9 app-flash
if errorlevel 1 (
  echo.
  echo FLASH FAILED. Build already succeeded; do not rebuild.
  echo Retry:
  echo   idf.py -p COM9 app-flash
  exit /b 6
)

echo.
echo [3/3] MONITOR COM9
echo Expected first screen: MACHINE SELECT
echo   default: PDP-7
echo   Down:    MicroVAX II
echo Enter then selects OS.
echo PDP-7 / UNIX V0 must continue into the existing Release Final path.
echo MicroVAX II / 4.3BSD must stop at the S0 controlled placeholder.
echo.
idf.py -p COM9 monitor
set "RC=%ERRORLEVEL%"

echo.
echo Monitor ended with code %RC%.
echo Ctrl+C monitor termination is not a firmware failure.
exit /b %RC%
