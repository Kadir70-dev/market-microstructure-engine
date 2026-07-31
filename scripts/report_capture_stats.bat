@echo off
setlocal EnableDelayedExpansion
if "%~1"=="" (
  echo usage: %~nx0 ^<wal-directory^> [capture-log]
  exit /b 2
)
for /f "delims=" %%I in ('wsl.exe wslpath -a "%~dp0report_capture_stats.sh"') do set "MME_SCRIPT=%%I"
for /f "delims=" %%I in ('wsl.exe wslpath -a "%~1"') do set "MME_WAL=%%I"
if "%~2"=="" (
  wsl.exe -d Ubuntu -- bash "%MME_SCRIPT%" "%MME_WAL%"
) else (
  for /f "delims=" %%I in ('wsl.exe wslpath -a "%~2"') do set "MME_LOG=%%I"
  wsl.exe -d Ubuntu -- bash "%MME_SCRIPT%" "%MME_WAL%" "!MME_LOG!"
)
exit /b %ERRORLEVEL%
