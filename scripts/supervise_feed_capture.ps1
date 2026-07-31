# supervise_feed_capture.ps1
#
# External supervisor for the Phase 2c named-pipe recorder.
#
# Scope: this process manages the recorder from OUTSIDE. It does not modify the
# recorder, the wire protocol, or the WAL format. Architecture Version 1.0
# places an in-process supervisor thread in Phase 9 (Part 10.4, control/
# supervisor); Phase 2 has no such component, and Part 21 assigns recorder
# restart to the service manager ("systemd restart"). On Windows there is no
# systemd, so this script is that service manager.
#
# Fail-closed model, mirroring the durable kill state of Part 10.4: any
# condition that Part 21 classifies as "halt" writes a persistent halt file and
# the supervisor refuses to restart until a human clears it. Restarting through
# a sequence gap would silently manufacture an unusable recording interval,
# which is precisely what Part 21 forbids.
#
# Detected conditions:
#   recorder termination      -> restart (bounded)
#   stale recording           -> restart (bounded); heartbeats make this
#                                market-hours independent
#   sequence gap              -> HALT, no restart          (Part 21)
#   WAL write / recovery fail -> HALT, no restart          (Part 21)
#   handshake reject          -> HALT, no restart          (Part 5.3 clause 3)
#   named-pipe disconnect     -> observed as termination, restart
#
# Usage: set the same environment variables as run_feed_capture.bat, then run.
#   MME_RECORDER MME_PIPE MME_WAL_DIR MME_ACCOUNT MME_MARGIN_MODE
#   MME_SERVER_HASH MME_SYMBOL_HASH MME_RUN_SEED
# Optional: MME_STATE_DIR MME_POLL_SECONDS MME_STALE_SECONDS
#           MME_MAX_RESTARTS MME_RESTART_WINDOW_SECONDS
#
# MME_EPOCH is deliberately NOT read from the environment. Part 5.3 requires a
# monotonic session_epoch; the supervisor issues a fresh one on every start so
# that any EA still holding a previous session is rejected as stale rather than
# silently accepted.

$ErrorActionPreference = 'Stop'

$required = @('MME_RECORDER','MME_PIPE','MME_WAL_DIR','MME_ACCOUNT','MME_MARGIN_MODE',
              'MME_SERVER_HASH','MME_SYMBOL_HASH','MME_RUN_SEED')
foreach ($name in $required) {
    if (-not (Test-Path "env:$name")) {
        # Written directly to stderr: Write-Error under ErrorActionPreference=Stop
        # would throw before the documented exit code could be returned.
        [Console]::Error.WriteLine("ERROR missing required environment variable $name")
        exit 2
    }
}

$recorder = $env:MME_RECORDER
$walDir   = $env:MME_WAL_DIR
$logDir   = Join-Path $walDir 'logs'
if ($env:MME_STATE_DIR) { $stateDir = $env:MME_STATE_DIR } else { $stateDir = Join-Path $walDir 'state' }

if ($env:MME_POLL_SECONDS)           { $pollSeconds    = [int]$env:MME_POLL_SECONDS }           else { $pollSeconds    = 10 }
if ($env:MME_STALE_SECONDS)          { $staleSeconds   = [int]$env:MME_STALE_SECONDS }          else { $staleSeconds   = 180 }
if ($env:MME_MAX_RESTARTS)           { $maxRestarts    = [int]$env:MME_MAX_RESTARTS }           else { $maxRestarts    = 5 }
if ($env:MME_RESTART_WINDOW_SECONDS) { $restartWindow  = [int]$env:MME_RESTART_WINDOW_SECONDS } else { $restartWindow  = 600 }

foreach ($dir in @($walDir, $logDir, $stateDir)) {
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
}

$haltFile   = Join-Path $stateDir 'recorder_halt.json'
$healthFile = Join-Path $stateDir 'recorder_health.json'
$superLog   = Join-Path $logDir  'supervisor.log'

function Write-Line([string]$text) {
    $stamp = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    $line = "$stamp $text"
    Write-Output $line
    Add-Content -Path $superLog -Value $line -Encoding utf8
}

function Write-Health($state, $reason, $procId, $events, $gaps, $restarts) {
    $payload = [ordered]@{
        timestamp_utc  = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        state          = $state
        reason         = $reason
        pid            = $procId
        total_events   = [Math]::Max([int]$events, 0)
        sequence_gaps  = $gaps
        restarts       = $restarts
        wal_dir        = $walDir
    }
    $payload | ConvertTo-Json | Set-Content -Path $healthFile -Encoding utf8
}

# Part 10.4 pattern: a persisted halt starts the supervisor halted regardless of
# configuration. Clearing it is a manual, logged human action.
if (Test-Path $haltFile) {
    $existing = Get-Content $haltFile -Raw
    Write-Line "SUPERVISOR_HALTED_AT_START halt_file=$haltFile"
    Write-Line $existing
    Write-Health 'HALTED' 'halt_file_present_at_start' 0 0 0 0
    exit 3
}

function Set-Halt([string]$reason, [string]$detail) {
    $payload = [ordered]@{
        timestamp_utc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        reason        = $reason
        detail        = $detail
        cleared_by    = $null
    }
    $payload | ConvertTo-Json | Set-Content -Path $haltFile -Encoding utf8
    Write-Line "HALT reason=$reason detail=$detail action=manual_clear_required file=$haltFile"
}

$restartTimes = New-Object System.Collections.ArrayList
$totalRestarts = 0

while ($true) {

    # Part 5.3: monotonic session_epoch, fresh on every start.
    $epoch = [uint64][Math]::Floor(((Get-Date).ToUniversalTime() - (Get-Date '1970-01-01T00:00:00Z').ToUniversalTime()).TotalSeconds)
    $stamp = (Get-Date).ToString('yyyyMMdd_HHmmss')
    $captureLog = Join-Path $logDir "capture_$stamp.log"

    $argList = @($env:MME_PIPE, $walDir, $env:MME_ACCOUNT, "$epoch", $env:MME_MARGIN_MODE,
                 $env:MME_SERVER_HASH, $env:MME_SYMBOL_HASH, $env:MME_RUN_SEED)

    $proc = Start-Process -FilePath $recorder -ArgumentList $argList -PassThru -WindowStyle Hidden `
                          -RedirectStandardOutput $captureLog -RedirectStandardError "$captureLog.err"
    Write-Line "RECORDER_START pid=$($proc.Id) session_epoch=$epoch log=$captureLog"
    Write-Health 'AWAITING_CONNECTION' 'recorder_started' $proc.Id 0 0 $totalRestarts

    $connected = $false
    $lastEvents = -1
    $lastGrowth = Get-Date
    $exitReason = 'unknown'

    while ($true) {
        Start-Sleep -Seconds $pollSeconds
        try { $proc.Refresh() } catch { }

        $text = ''
        if (Test-Path $captureLog) { $text = Get-Content $captureLog -Raw -ErrorAction SilentlyContinue }
        if (-not $text) { $text = '' }

        # --- Fail-closed markers. Part 21 classifies each of these as halt. ---
        $halted = $null
        if ($text -match 'HANDSHAKE_REJECT')            { $halted = 'handshake_reject' }
        elseif ($text -match 'RECOVERY_FAIL')           { $halted = 'wal_recovery_failure' }
        elseif ($text -match 'RECOVERY_COMPRESSION_FAIL') { $halted = 'wal_compression_failure' }
        elseif ($text -match 'RECORDER_HALT')           { $halted = 'wal_write_failure' }
        elseif ($text -match 'INGRESS_HALT')            { $halted = 'ingress_halt' }

        # --- Live counters straight from the recorder's own reporting. ---
        $events = $lastEvents
        $gaps = 0
        $statLines = [regex]::Matches($text, 'CAPTURE_STATS total_events=(\d+).*?sequence_gaps=(\d+)')
        if ($statLines.Count -gt 0) {
            $last = $statLines[$statLines.Count - 1]
            $events = [int]$last.Groups[1].Value
            $gaps = [int]$last.Groups[2].Value
        }
        if ($text -match 'PIPE_CONNECTED') { $connected = $true }

        # A gap is unusable data, not a transient. Never restart through it.
        if ($gaps -gt 0 -and -not $halted) { $halted = 'sequence_gap' }

        if ($halted) {
            Set-Halt $halted "capture_log=$captureLog"
            if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
            Write-Health 'HALTED' $halted $proc.Id $events $gaps $totalRestarts
            exit 3
        }

        if ($events -gt $lastEvents) { $lastEvents = $events; $lastGrowth = Get-Date }

        if ($proc.HasExited) { $exitReason = "recorder_exit_code=$($proc.ExitCode)"; break }

        # Staleness is only meaningful once a client has connected. Before that
        # the recorder is legitimately blocked in ConnectNamedPipe.
        if ($connected) {
            $idle = ((Get-Date) - $lastGrowth).TotalSeconds
            if ($idle -ge $staleSeconds) { $exitReason = "stale_no_events_for_$([int]$idle)s"; break }
            Write-Health 'HEALTHY' 'ok' $proc.Id $lastEvents $gaps $totalRestarts
        } else {
            Write-Health 'AWAITING_CONNECTION' 'no_client_yet' $proc.Id 0 0 $totalRestarts
        }
    }

    Write-Line "RECORDER_DOWN reason=$exitReason events=$lastEvents"
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }

    # Crash-loop guard. Repeated failure is a condition for a human, not a loop.
    $now = Get-Date
    [void]$restartTimes.Add($now)
    $cutoff = $now.AddSeconds(-$restartWindow)
    $recent = @($restartTimes | Where-Object { $_ -gt $cutoff })
    $restartTimes.Clear()
    foreach ($t in $recent) { [void]$restartTimes.Add($t) }

    if ($recent.Count -gt $maxRestarts) {
        Set-Halt 'restart_loop' "restarts=$($recent.Count) window_seconds=$restartWindow"
        Write-Health 'HALTED' 'restart_loop' 0 $lastEvents 0 $totalRestarts
        exit 3
    }

    $totalRestarts = $totalRestarts + 1
    Write-Health 'RESTARTING' $exitReason 0 $lastEvents 0 $totalRestarts
    Write-Line "RECORDER_RESTART attempt=$totalRestarts recent_in_window=$($recent.Count)/$maxRestarts"
    Start-Sleep -Seconds 2
}
