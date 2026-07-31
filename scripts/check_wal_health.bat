@echo off
setlocal
if "%~1"=="" (
  echo usage: %~nx0 ^<wal-directory^>
  exit /b 2
)
for /f "delims=" %%I in ('wsl.exe wslpath -a "%~dp0check_wal_health.sh"') do set "MME_SCRIPT=%%I"
for /f "delims=" %%I in ('wsl.exe wslpath -a "%~1"') do set "MME_WAL=%%I"
wsl.exe -d Ubuntu -- bash "%MME_SCRIPT%" "%MME_WAL%"
exit /b %ERRORLEVEL%
