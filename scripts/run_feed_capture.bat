@echo off
setlocal

for %%V in (MME_RECORDER MME_PIPE MME_WAL_DIR MME_ACCOUNT MME_EPOCH MME_MARGIN_MODE MME_SERVER_HASH MME_SYMBOL_HASH MME_RUN_SEED) do (
  if not defined %%V (
    echo ERROR missing required environment variable %%V 1>&2
    exit /b 2
  )
)

if not defined MME_LOG_DIR set "MME_LOG_DIR=%MME_WAL_DIR%\logs"
if not exist "%MME_WAL_DIR%" mkdir "%MME_WAL_DIR%" || exit /b 1
if not exist "%MME_LOG_DIR%" mkdir "%MME_LOG_DIR%" || exit /b 1
for /f %%I in ('powershell.exe -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "MME_STAMP=%%I"
set "MME_CAPTURE_LOG=%MME_LOG_DIR%\capture_%MME_STAMP%.log"

echo Starting named-pipe recorder. Attach mme_feed.mq5 only after PIPE_CONNECTED is possible.
echo Log: %MME_CAPTURE_LOG%
"%MME_RECORDER%" "%MME_PIPE%" "%MME_WAL_DIR%" "%MME_ACCOUNT%" "%MME_EPOCH%" "%MME_MARGIN_MODE%" "%MME_SERVER_HASH%" "%MME_SYMBOL_HASH%" "%MME_RUN_SEED%" >> "%MME_CAPTURE_LOG%" 2>&1
set "MME_EXIT=%ERRORLEVEL%"
echo Recorder exit code: %MME_EXIT%
echo Review: "%MME_CAPTURE_LOG%"
exit /b %MME_EXIT%
