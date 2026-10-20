#Requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$TesseraCli,
    [Parameter(Mandatory)][string]$EngineWorker,
    [Parameter(Mandatory)][string]$EvidenceDirectory,
    [ValidateRange(10, 600)][int]$TimeoutSeconds = 120
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$evidence = [IO.Path]::GetFullPath($EvidenceDirectory)
$runRoot = Join-Path $evidence ('t018-{0}-{1}' -f [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'), [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($runRoot) | Out-Null
$instances = [Collections.Generic.HashSet[string]]::new()
$runs = [Collections.Generic.List[object]]::new()
$summary = [ordered]@{
    schema_version = 1; scope = 'Gate 2 synthetic real Engine facade; no checkpoints, real packs, Runtime or MVP acceptance'
    started_utc = [DateTime]::UtcNow.ToString('o'); passed = $false; build_jobs = 1
    runner_sha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
    engine_frames = 0; autonomous_frames = 0; matrix_runs = 0; first_divergence = $null
    runs = $runs; errors = @(); actions_environment = ($env:GITHUB_ACTIONS -eq 'true')
}
function Assert-Valid([bool]$Condition, [string]$Code) {
    if (-not $Condition) { throw $Code }
}
function Assert-Hash($Value, [string]$Code) {
    Assert-Valid (($Value -is [string]) -and $Value -cmatch '^[a-f0-9]{64}$') $Code
}
function Hash-File([string]$Path) {
    Assert-Valid (Test-Path -LiteralPath $Path -PathType Leaf) "REQUIRED_FILE_MISSING: $Path"
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Instance-Key([int]$ProcessId, [uint64]$Birth) { return '{0}/{1}' -f $ProcessId, $Birth }
function Read-Report([string]$Path) {
    Assert-Valid ((Get-Item -LiteralPath $Path).Length -le 32MB) "REPORT_LIMIT_EXCEEDED: $Path"
    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -Depth 100
}
function Invoke-Child([string]$Name, [string]$Executable, [string[]]$Arguments, [string]$ReportPath) {
    Assert-Valid (-not (Test-Path -LiteralPath $ReportPath)) "STALE_REPORT_ALREADY_EXISTS: $Name"
    $beforeHash = Hash-File $Executable
    $settings = [Diagnostics.ProcessStartInfo]::new()
    $settings.FileName = $Executable; $settings.WorkingDirectory = $runRoot
    $settings.UseShellExecute = $false; $settings.CreateNoWindow = $true
    $settings.RedirectStandardOutput = $true; $settings.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $settings.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new(); $process.StartInfo = $settings
    try {
        Assert-Valid ($process.Start()) "PROCESS_START_FAILED: $Name"
        $childId = $process.Id; $birth = [uint64]$process.StartTime.ToUniversalTime().ToFileTimeUtc()
        $instance = Instance-Key $childId $birth
        Assert-Valid ($childId -ne $PID -and $instances.Add($instance)) "PROCESS_INSTANCE_REUSED: $Name"
        $outTask = $process.StandardOutput.ReadToEndAsync(); $errTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $process.Kill($true); $process.WaitForExit(); throw "PROCESS_TIMEOUT: $Name"
        }
        $stdout = $outTask.GetAwaiter().GetResult(); $stderr = $errTask.GetAwaiter().GetResult()
        [IO.File]::WriteAllText((Join-Path $runRoot "$Name.stdout.log"), $stdout)
        [IO.File]::WriteAllText((Join-Path $runRoot "$Name.stderr.log"), $stderr)
        Assert-Valid ($process.ExitCode -eq 0) "PROCESS_FAILED: $Name exit=$($process.ExitCode) $stderr"
        Assert-Valid ((Hash-File $Executable) -ceq $beforeHash) "EXECUTABLE_CHANGED_DURING_RUN: $Name"
        $value = Read-Report $ReportPath
        Assert-Valid ($value.code -eq 0 -and $value.pid -eq $childId) "REPORT_PROCESS_OR_STATUS_INVALID: $Name"
        Assert-Valid ([uint64]$value.process_creation_time_utc_100ns -eq $birth) "REPORT_OS_CREATION_MISMATCH: $Name"
        $meta = [ordered]@{name=$Name; process_id=$childId; creation_utc_100ns=$birth; process_instance=$instance; executable_sha256=$beforeHash; report=$ReportPath}
        $runs.Add($meta)
        return [pscustomobject]@{Value=$value; Meta=$meta}
    } finally { $process.Dispose() }
}
function Verify-Series($Series, [string]$Name) {
    Assert-Valid ($Series.unit -ceq 'microseconds' -and $Series.count -eq 3000 -and $Series.samples.Count -eq 3000) "COST_SERIES_INVALID: $Name"
    foreach ($sample in $Series.samples) {
        Assert-Valid ([double]::IsFinite([double]$sample) -and [double]$sample -ge 0) "COST_SAMPLE_INVALID: $Name"
    }
    $ordered = @($Series.samples | Sort-Object)
    foreach ($percent in @(50,95,99)) {
        $index = [int][Math]::Ceiling(3000.0 * $percent / 100.0) - 1
        Assert-Valid ([double]$Series."p$percent" -eq [double]$ordered[$index]) "COST_PERCENTILE_INVALID: $Name p$percent"
    }
}
function Verify-Replay($Run, [string]$Backend, [bool]$Action, [string]$Observer, [bool]$Engine) {
    $v = $Run.Value; $name = $Run.Meta.name
    Assert-Valid ($v.requested_frames -eq 3000 -and $v.completed_frames -eq 3000 -and $v.frames.Count -eq 3000 -and $v.eof_code -eq 1 -and $v.closed -eq $true) "INCOMPLETE_REPLAY: $name"
    Assert-Valid ($v.backend.id -ceq $Backend -and $v.backend.kind -eq 2 -and $v.backend.system -eq 0) "BACKEND_IDENTITY_INVALID: $name"
    foreach ($key in @('build_id','content','config','trace','initial_state')) { Assert-Hash $v.$key "IDENTITY_HASH_INVALID: $name $key" }
    Assert-Valid ($v.backend.build_id -ceq $v.build_id -and $v.clock.tick_hz_num -eq 60000 -and $v.clock.tick_hz_den -eq 1) "BACKEND_CLOCK_INVALID: $name"
    $reads = if ($Observer -eq 'off') { 0 } else { 6000 }
    Assert-Valid ($v.observer.mode -ceq $Observer -and $v.observer.snapshot_reads -eq $reads -and $v.observer.event_batches -eq $reads) "OBSERVATION_NOT_EXECUTED: $name"
    if ($Engine) {
        Assert-Valid ($v.real_facade -eq $true -and $v.route -ceq 'AytherSession->NativeEmulation->Tessera') "REAL_FACADE_MISSING: $name"
        Assert-Valid ($v.process_instance -ceq $Run.Meta.process_instance) "EXACT_PROCESS_IDENTITY_INVALID: $name"
        Assert-Valid ($v.functional_oracles.state -eq 3000 -and $v.functional_oracles.video -eq 3000 -and $v.functional_oracles.audio -eq 3000) "INDEPENDENT_ORACLES_NOT_EXECUTED: $name"
        $actionCount = if ($Action) { 300 } else { 0 }
        Assert-Valid ($v.functional_oracles.action_frames -eq $actionCount -and $v.functional_oracles.neutral_frames -eq (3000-$actionCount)) "INPUT_ORACLE_COVERAGE_INVALID: $name"
        Assert-Valid ($v.lifecycle.step_completed -eq 1 -and $v.lifecycle.reset_completed -eq 1 -and $v.lifecycle.preflight -eq $true) "LIFECYCLE_PREFLIGHT_NOT_EXECUTED: $name"
        Assert-Valid ($v.oracle_provenance -match '0651aad31eb02b6f9608dbfa825b5f300f537385e8ebc611db52022424fe8558') "ORACLE_PROVENANCE_MISSING: $name"
        Verify-Series $v.costs.functional_oracle "$name functional_oracle"
    }
    foreach ($stage in @('step','canonical_hashes','observation')) { Verify-Series $v.costs.$stage "$name $stage" }
    for ($index=0; $index -lt 3000; ++$index) {
        $frame=$v.frames[$index]; Assert-Valid ($frame.frame -eq $index+1) "FRAME_SEQUENCE_INVALID: $name"
        foreach ($domain in @('state','video','audio')) { Assert-Hash $frame.$domain "FRAME_HASH_INVALID: $name frame=$($index+1) $domain" }
    }
}
function Compare-Replay($Baseline, $Other) {
    Assert-Valid ($Baseline.Meta.process_instance -cne $Other.Meta.process_instance) 'REPLAY_NOT_INDEPENDENT_PROCESS'
    foreach ($key in @('build_id','content','config','trace','initial_state')) {
        Assert-Valid ($Baseline.Value.$key -ceq $Other.Value.$key) "REPLAY_IDENTITY_MISMATCH: $($Other.Meta.name) $key"
    }
    for ($index=0; $index -lt 3000; ++$index) {
        foreach ($domain in @('state','video','audio')) {
            $expected=$Baseline.Value.frames[$index].$domain; $actual=$Other.Value.frames[$index].$domain
            if ($expected -cne $actual) {
                $summary.first_divergence=@{run=$Other.Meta.name;frame=$index+1;domain=$domain;expected=$expected;actual=$actual}
                throw "CANONICAL_DIVERGENCE: $($Other.Meta.name) frame=$($index+1) $domain"
            }
        }
    }
}
try {
    $TesseraCli = [IO.Path]::GetFullPath($TesseraCli); $EngineWorker = [IO.Path]::GetFullPath($EngineWorker)
    $summary.tessera_cli_sha256=Hash-File $TesseraCli; $summary.engine_worker_sha256=Hash-File $EngineWorker
    Assert-Valid ((Instance-Key 42 1) -cne (Instance-Key 42 2)) 'RECYCLED_PID_REGRESSION'
    Assert-Valid ((Instance-Key 42 1) -ceq (Instance-Key 42 1)) 'SAME_PROCESS_REGRESSION'
    Assert-Valid ((Instance-Key 42 1) -cne (Instance-Key 43 1)) 'DIFFERENT_PROCESS_REGRESSION'
    $selfPath=Join-Path $runRoot 'facade-self-tests.json'
    $self=Invoke-Child 'facade-self-tests' $EngineWorker @('--self-test','--report',$selfPath) $selfPath
    Assert-Valid ($self.Value.self_tests.passed -eq $true -and $self.Value.self_tests.checks -ge 40 -and $self.Value.self_tests.real_facade -eq $true) 'REAL_FACADE_SELF_TESTS_NOT_EXECUTED'
    foreach ($proof in @('retained_after_owner_destruction','legacy_calls_rejected_without_advance','device_and_pack_rejected',
        'complete_input_negatives','driver_reset_close_views_cleared','snapshot_budget_and_recovery',
        'queue_4096_independent_cursors_and_losses','saturated_reset_close_cancellation')) {
        Assert-Valid ($self.Value.self_tests.$proof -eq $true) "FACADE_CONTRACT_PROOF_MISSING: $proof"
    }
    $summary.self_tests=$self.Value.self_tests
    $baselines=@{}; $engineRuns=[Collections.Generic.List[object]]::new()
    foreach ($profile in @('minimal','complete')) {
        foreach ($scenario in @('neutral','action')) {
            $key="$profile-$scenario"; $backend="synthetic.$profile"; $action=$scenario -eq 'action'
            $fixture=Join-Path $runRoot "fixtures-$key"; $baselinePath=Join-Path $runRoot "autonomous-$key.json"
            $baseline=Invoke-Child "autonomous-$key" $TesseraCli @('demo','--backend',$backend,'--scenario',$scenario,'--frames','3000','--observer','off','--fixture-dir',$fixture,'--report',$baselinePath) $baselinePath
            Verify-Replay $baseline $backend $action 'off' $false
            foreach ($entry in @(@('content','content.bin'),@('config','config.bin'),@('trace','trace.bin'))) {
                Assert-Valid ((Hash-File (Join-Path $fixture $entry[1])) -ceq $baseline.Value.($entry[0])) "FIXTURE_HASH_MISMATCH: $key $($entry[0])"
            }
            $trace=Read-Report (Join-Path $fixture 'trace.json')
            Assert-Valid ($trace.length -eq 3000 -and $trace.frames.Count -eq 3000) "FIXTURE_TRACE_LENGTH_INVALID: $key"
            for ($index=0; $index -lt 3000; ++$index) {
                $frame=$trace.frames[$index]; Assert-Valid ($frame.frame -eq $index+1 -and $frame.assignments.Count -eq 8) "FIXTURE_TRACE_SEQUENCE_INVALID: $key"
                for ($control=0; $control -lt 8; ++$control) {
                    $expected = if ($action -and $index -ge 300 -and $index -lt 600 -and $control -eq 5) {1} else {0}
                    $item=$frame.assignments[$control]
                    Assert-Valid ($item.port -eq 0 -and $item.control -eq $control -and $item.value -eq $expected) "FIXTURE_INPUT_ORACLE_INVALID: $key frame=$($index+1)"
                }
            }
            $baselines[$key]=$baseline; $summary.autonomous_frames += $baseline.Value.completed_frames
            $modes=if ($profile -eq 'complete') {@('off','two-readers')} else {@('off')}
            foreach ($mode in $modes) { for ($repeat=0; $repeat -lt 2; ++$repeat) {
                $name="engine-$key-$mode-$repeat"; $path=Join-Path $runRoot "$name.json"
                $run=Invoke-Child $name $EngineWorker @('--worker','--backend',$backend,'--scenario',$scenario,'--observer',$mode,'--fixture-dir',$fixture,'--report',$path) $path
                Verify-Replay $run $backend $action $mode $true; Compare-Replay $baseline $run
                $summary.engine_frames += $run.Value.completed_frames; ++$summary.matrix_runs; $engineRuns.Add($run)
            }}
        }
    }
    foreach ($profile in @('minimal','complete')) {
        foreach ($domain in @('state','video','audio')) {
            Assert-Valid ($baselines["$profile-neutral"].Value.frames[449].$domain -cne $baselines["$profile-action"].Value.frames[449].$domain) "INPUT_ACTION_DID_NOT_CHANGE_DOMAIN: $profile $domain"
        }
    }
    Assert-Valid ($summary.matrix_runs -eq 12 -and $summary.engine_frames -eq 36000 -and $summary.autonomous_frames -eq 12000) 'MATRIX_COVERAGE_INCOMPLETE'
    Assert-Valid ((Hash-File $EngineWorker) -ceq $summary.engine_worker_sha256 -and (Hash-File $TesseraCli) -ceq $summary.tessera_cli_sha256) 'MATRIX_BINARY_CHANGED'
    $summary.process_instances=$instances.Count; $summary.divergences=0; $summary.passed=$true
} catch { $summary.errors=@($_.Exception.Message) }
finally {
    $summary.finished_utc=[DateTime]::UtcNow.ToString('o')
    $json=$summary | ConvertTo-Json -Depth 100
    [IO.File]::WriteAllText((Join-Path $runRoot 'summary.json'),$json)
    [IO.File]::WriteAllText((Join-Path $evidence 'tessera-native-summary.json'),$json)
}
if (-not $summary.passed) { Write-Error ($summary.errors -join '; '); exit 1 }
Write-Output "Gate 2: 12 procesos Engine, 36.000 frames y cero divergencias; evidencia $runRoot"
exit 0
