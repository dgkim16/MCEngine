<#
.SYNOPSIS
    The per-commit gate: build, audit, capture sweep, capture and FG-dump diff, D3D12 log, tests.

.DESCRIPTION
    verify.ps1 -Baseline <dir> [-Tier quick|full] [-Poison <pass|*>] [-SkipBuild] [-NoTests] [-UpdateBaseline]

    1. Unless -SkipBuild, rebuilds Debug x64 and Release x64 (MSBuild from vswhere, else $env:MSBUILD)
       and fails if either configuration prints more compiler/linker warnings than
       <Baseline>\warnings.txt allows.
    2. Runs Tools\audit.ps1, which fails on enforced rules and reports the rest.
    3. Runs x64\Debug\MC_main.exe with the engine directory as working directory:
           --capture-sweep <work>\<tier> --tier <tier> --verify-log <work>\<tier>\d3d12.log [--poison <p>]
    4. Compares the BMPs with <Baseline>\<tier> through compare_captures.ps1 (mask <Baseline>\mask.txt
       when it exists), and the .fg.txt dumps byte for byte.
    5. Fails on any ERROR or CORRUPTION line in d3d12.log, and if its live-object section lists more
       lines than <Baseline>\live_objects.txt.
    6. Unless -NoTests, runs --run-tests <work>\tests.txt --verify-log <work>\tests_d3d12.log and fails
       on a FAIL line, a non-zero exit code, a test from <Baseline>\tests.txt that no longer passes,
       or any D3D12 message before the live-object section (--verify-log turns break-on-severity off).
    The --verify-log file is also the engine's stderr: when the sweep or the test run fails, its
    other lines (a failed assert) are printed.
    Prints a one-screen summary; exit code 0 only if every step passed. <work> is x64\Verify under
    the engine directory; build logs, the audit and compare output land there too.

    -UpdateBaseline: after a run whose build, audit, sweep, log and tests are clean, writes the
    captures, FG dumps, timers and d3d12.log to <Baseline>\<tier>, the live-object section to
    <Baseline>\live_objects.txt and the test results to <Baseline>\tests.txt. Use it only for
    labelled changes, and log each use in <Baseline>\CHANGELOG.md. warnings.txt and mask.txt are
    maintained by hand.

    Baseline directory:
        warnings.txt        'Debug <n>' and 'Release <n>' lines
        mask.txt            compare_captures.ps1 -BuildMask output plus hand-written scatter lines (optional)
        live_objects.txt    the lines under '== LIVE OBJECTS ==' of a clean run ('#' lines ignored)
        tests.txt           the --run-tests output of a clean run
        quick\, full\       <scene>_<config>.bmp/.fg.txt/.timers.txt and d3d12.log per tier
#>
param(
    [Parameter(Mandatory = $true)] [string]$Baseline,
    [ValidateSet('quick', 'full')] [string]$Tier = 'quick',
    [string]$Poison,
    [switch]$SkipBuild,
    [switch]$NoTests,
    [switch]$UpdateBaseline
)

$ErrorActionPreference = 'Continue'   # native tools write to stderr; every step checks exit codes itself
$ProcessTimeoutSec = 300

$engineDir = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path.TrimEnd('\')
$solution = Join-Path $engineDir 'MC_Engine.sln'
$exe = Join-Path $engineDir 'x64\Debug\MC_main.exe'
$work = Join-Path $engineDir 'x64\Verify'
$sweepDir = Join-Path $work $(if ($Poison) { "$Tier-poison" } else { $Tier })
$logFile = Join-Path $sweepDir 'd3d12.log'
$testsFile = Join-Path $work 'tests.txt'
$testsLog = Join-Path $work 'tests_d3d12.log'

if (-not [System.IO.Path]::IsPathRooted($Baseline)) { $Baseline = Join-Path (Get-Location).Path $Baseline }
$Baseline = [System.IO.Path]::GetFullPath($Baseline).TrimEnd('\')
$baseTier = Join-Path $Baseline $Tier

$results = New-Object System.Collections.Generic.List[object]
function Set-Result([string]$step, [string]$status, [string]$detail) {
    $results.Add([pscustomobject]@{ Step = $step; Status = $status; Detail = $detail })
    Write-Output ('  {0,-14} {1,-7} {2}' -f $step, $status, $detail)
}
function Show-Lines([object[]]$lines, [int]$max = 20) {
    $shown = @($lines | Select-Object -First $max)
    foreach ($l in $shown) { Write-Output "      $l" }
    if (@($lines).Count -gt $max) { Write-Output "      ... $(@($lines).Count - $max) more" }
}
function Invoke-Script([string]$script, [string[]]$arguments, [string]$outFile) {
    $out = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot $script) @arguments 2>&1 |
             ForEach-Object { "$_" })
    $code = $LASTEXITCODE
    [System.IO.File]::WriteAllLines($outFile, [string[]]$out)
    return @{ Code = $code; Lines = $out }
}
function Invoke-Engine([string[]]$arguments) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = ($arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $psi.WorkingDirectory = $engineDir
    $psi.UseShellExecute = $false
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p = [System.Diagnostics.Process]::Start($psi)
    if (-not $p.WaitForExit($ProcessTimeoutSec * 1000)) {
        try { $p.Kill() } catch { }
        $p.WaitForExit(10000) | Out-Null
        return @{ TimedOut = $true; Code = $null; Seconds = $sw.Elapsed.TotalSeconds }
    }
    return @{ TimedOut = $false; Code = $p.ExitCode; Seconds = $sw.Elapsed.TotalSeconds }
}
function Format-ExitCode($code) { if ($code -lt 0) { '0x{0:X8}' -f $code } else { "$code" } }
function Clear-Outputs([string]$dir) {
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null; return }
    Get-ChildItem -LiteralPath $dir -File | Where-Object { $_.Name -match '\.(bmp|txt|log)$' } | Remove-Item -Force
}
function Read-LiveSection([string[]]$lines) {
    $i = [Array]::IndexOf($lines, '== LIVE OBJECTS ==')
    if ($i -lt 0) { return $null }
    return , @($lines | Select-Object -Skip ($i + 1) | Where-Object { $_.Trim() })   # an empty section stays an array
}
# The lines of a --verify-log file before its live-object section.
function Read-LogBody([string]$file) {
    if (-not (Test-Path -LiteralPath $file)) { return , @() }
    $lines = [System.IO.File]::ReadAllLines($file)
    $i = [Array]::IndexOf($lines, '== LIVE OBJECTS ==')
    return , @(if ($i -lt 0) { $lines } else { $lines | Select-Object -First $i })
}
# What the engine wrote to stderr (a failed assert, a CRT report), as opposed to D3D12 messages.
function Select-Stderr([object[]]$body) { @($body | Where-Object { $_.Trim() -and $_ -cnotmatch '^D3D12 ' }) }

Write-Output "verify: tier $Tier$(if ($Poison) { ", poison '$Poison'" }), baseline $Baseline"
if ($UpdateBaseline -and $Poison) { Write-Output 'verify: -UpdateBaseline never records a poisoned run'; exit 2 }
if (-not (Test-Path -LiteralPath $Baseline -PathType Container)) {
    if (-not $UpdateBaseline) { Write-Output "verify: no baseline directory $Baseline"; exit 2 }
    New-Item -ItemType Directory -Force -Path $Baseline | Out-Null
}
New-Item -ItemType Directory -Force -Path $work | Out-Null

# ---------------------------------------------------------------- 1. build
$buildOk = $true
if ($SkipBuild) {
    Set-Result 'build' 'SKIP' '-SkipBuild'
}
else {
    $allowed = @{}
    $warningsFile = Join-Path $Baseline 'warnings.txt'
    if (Test-Path -LiteralPath $warningsFile) {
        foreach ($l in [System.IO.File]::ReadAllLines($warningsFile)) {
            if ($l -match '^\s*(Debug|Release)\s*[:=]?\s*(\d+)') { $allowed[$Matches[1]] = [int]$Matches[2] }
        }
    }
    $msbuild = $null
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $msbuild = @(& $vswhere -latest -prerelease -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' 2>$null) |
                   Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
    }
    if (-not $msbuild -and $env:MSBUILD -and (Test-Path -LiteralPath $env:MSBUILD)) { $msbuild = $env:MSBUILD }
    if (-not $msbuild) {
        Set-Result 'build' 'FAIL' 'MSBuild not found (vswhere, $env:MSBUILD)'
        $buildOk = $false
    }
    foreach ($cfg in @('Debug', 'Release')) {
        if (-not $buildOk) { break }
        $out = @(& $msbuild $solution '-t:Restore;Rebuild' "/p:Configuration=$cfg" '/p:Platform=x64' '/m' '/nologo' '/v:minimal' 2>&1 |
                 ForEach-Object { "$_" })
        $code = $LASTEXITCODE
        [System.IO.File]::WriteAllLines((Join-Path $work "build_$cfg.log"), [string[]]$out)
        $warnings = @($out | Where-Object { $_ -cmatch '\bwarning [A-Z]+\d+\s*:' })
        $errors = @($out | Where-Object { $_ -cmatch '\berror [A-Z]*\d*\s*:' })
        $limit = $allowed[$cfg]
        if ($code -ne 0 -or $errors.Count) {
            Set-Result "build $cfg" 'FAIL' "exit $code, $($errors.Count) error line(s)"
            Show-Lines ($errors | ForEach-Object { ($_ -replace '\s*\[[^\]]*\.vcxproj\]$', '').Replace("$engineDir\", '').Trim() })
            $buildOk = $false
        }
        elseif ($null -eq $limit) {
            Set-Result "build $cfg" 'FAIL' "$($warnings.Count) warning(s); no '$cfg <n>' line in $warningsFile"
            $buildOk = $false
        }
        elseif ($warnings.Count -gt $limit) {
            Set-Result "build $cfg" 'FAIL' "$($warnings.Count) warning(s), baseline $limit"
            Show-Lines ($warnings | ForEach-Object { ($_ -replace '\s*\[[^\]]*\.vcxproj\]$', '').Replace("$engineDir\", '').Trim() })
            $buildOk = $false
        }
        else {
            Set-Result "build $cfg" 'PASS' "$($warnings.Count) warning(s), baseline $limit"
        }
    }
}
if (-not $buildOk) {
    Write-Output 'verify: FAIL (build; later steps not run)'
    exit 1
}

# ---------------------------------------------------------------- 2. audit
$audit = Invoke-Script 'audit.ps1' @() (Join-Path $work 'audit.log')
if ($audit.Code -eq 0) {
    Set-Result 'audit' 'PASS' (@($audit.Lines | Where-Object { $_ -like 'audit: * enforced violation*' })[0] -replace '^audit: ', '')
}
else {
    Set-Result 'audit' 'FAIL' "exit $($audit.Code)"
    Show-Lines @($audit.Lines | Where-Object { $_ -match ': error A\d+|^audit: ' })
}

# ---------------------------------------------------------------- 3. capture sweep
$sweepOk = $false
if (-not (Test-Path -LiteralPath $exe)) {
    Set-Result 'sweep' 'FAIL' "no $exe"
}
else {
    Clear-Outputs $sweepDir
    $engineArgs = @('--capture-sweep', $sweepDir, '--tier', $Tier, '--verify-log', $logFile)
    if ($Poison) { $engineArgs += @('--poison', $Poison) }
    $run = Invoke-Engine $engineArgs
    $bmps = @(Get-ChildItem -LiteralPath $sweepDir -Filter '*.bmp' -File)
    $detail = '{0} capture(s) in {1:N1} s' -f $bmps.Count, $run.Seconds
    if ($run.TimedOut) { Set-Result 'sweep' 'FAIL' "hung; killed after $ProcessTimeoutSec s; $detail" }
    elseif ($run.Code -ne 0) { Set-Result 'sweep' 'FAIL' "exit $(Format-ExitCode $run.Code); $detail" }
    elseif ($bmps.Count -eq 0) { Set-Result 'sweep' 'FAIL' "exit 0 but $detail" }
    else { Set-Result 'sweep' 'PASS' $detail; $sweepOk = $true }
    if (-not $sweepOk) { Show-Lines (Select-Stderr (Read-LogBody $logFile)) 10 }
}

# ---------------------------------------------------------------- 4. captures and FG dumps
$haveBaseline = (Test-Path -LiteralPath $baseTier) -and @(Get-ChildItem -LiteralPath $baseTier -Filter '*.bmp' -File).Count -gt 0
$compareStatus = if ($UpdateBaseline) { 'INFO' } else { 'FAIL' }
if (-not $haveBaseline) {
    Set-Result 'captures' $(if ($UpdateBaseline) { 'SKIP' } else { 'FAIL' }) "no baseline captures in $baseTier"
    Set-Result 'fg dumps' $(if ($UpdateBaseline) { 'SKIP' } else { 'FAIL' }) "no baseline in $baseTier"
}
else {
    $compareArgs = @('-Baseline', $baseTier, '-Current', $sweepDir)
    $maskFile = Join-Path $Baseline 'mask.txt'
    if (Test-Path -LiteralPath $maskFile) { $compareArgs += @('-Mask', $maskFile) }
    $cmp = Invoke-Script 'compare_captures.ps1' $compareArgs (Join-Path $work "compare_$(Split-Path -Leaf $sweepDir).log")
    $last = @($cmp.Lines | Where-Object { $_ -like 'compare_captures: * differing pixel*' })
    $summary = if ($last.Count) { $last[-1] -replace '^compare_captures: ', '' } else { "exit $($cmp.Code)" }
    if ($cmp.Code -eq 0) { Set-Result 'captures' 'PASS' $summary }
    else {
        Set-Result 'captures' $compareStatus $summary
        Show-Lines @($cmp.Lines | Where-Object { $_ -match '^\s+\S' -and $_ -notmatch '\s0 differing' } | ForEach-Object { $_.Trim() })
    }

    $fgDiffs = New-Object System.Collections.Generic.List[string]
    $baseFg = @(Get-ChildItem -LiteralPath $baseTier -Filter '*.fg.txt' -File)
    foreach ($f in $baseFg) {
        $cur = Join-Path $sweepDir $f.Name
        if (-not (Test-Path -LiteralPath $cur)) { $fgDiffs.Add("$($f.Name): MISSING"); continue }
        $a = [System.IO.File]::ReadAllLines($f.FullName)
        $b = [System.IO.File]::ReadAllLines($cur)
        $n = [Math]::Max($a.Count, $b.Count)
        for ($i = 0; $i -lt $n; ++$i) {
            $la = if ($i -lt $a.Count) { $a[$i] } else { '<end of file>' }
            $lb = if ($i -lt $b.Count) { $b[$i] } else { '<end of file>' }
            if ($la -cne $lb) { $fgDiffs.Add("$($f.Name):$($i + 1): baseline '$la' / now '$lb'"); break }
        }
    }
    if ($baseFg.Count -eq 0) { Set-Result 'fg dumps' 'SKIP' "no .fg.txt in $baseTier" }
    elseif ($fgDiffs.Count -eq 0) { Set-Result 'fg dumps' 'PASS' "$($baseFg.Count) identical" }
    else {
        Set-Result 'fg dumps' $compareStatus "$($fgDiffs.Count) of $($baseFg.Count) differ"
        Show-Lines $fgDiffs 10
    }
}

# ---------------------------------------------------------------- 5. D3D12 log
$logOk = $false
$liveLines = $null
if (-not (Test-Path -LiteralPath $logFile)) {
    Set-Result 'd3d12 log' 'FAIL' "no $logFile"
}
else {
    $log = [System.IO.File]::ReadAllLines($logFile)
    $bad = @($log | Where-Object { $_ -cmatch '^(D3D12 )?(ERROR|CORRUPTION)\b' })
    $warn = @($log | Where-Object { $_ -cmatch '^D3D12 WARNING\b' }).Count
    $liveLines = Read-LiveSection $log
    $baseLiveFile = Join-Path $Baseline 'live_objects.txt'
    $baseLive = $null
    if (Test-Path -LiteralPath $baseLiveFile) {
        $baseLive = @([System.IO.File]::ReadAllLines($baseLiveFile) | Where-Object { $_.Trim() -and -not $_.StartsWith('#') }).Count
    }
    $liveText = if ($null -eq $liveLines) { 'no live-object section' } else { "live objects $($liveLines.Count) (baseline $(if ($null -eq $baseLive) { 'none' } else { $baseLive }))" }
    $detail = "$($bad.Count) error(s), $warn warning(s); $liveText"
    if ($log -contains '== NO D3D12 INFO QUEUE ==') { Set-Result 'd3d12 log' 'FAIL' 'the debug layer was not available' }
    elseif ($bad.Count) { Set-Result 'd3d12 log' 'FAIL' $detail; Show-Lines $bad 10 }
    elseif ($null -eq $liveLines) { Set-Result 'd3d12 log' 'FAIL' "$detail (the process did not reach teardown)" }
    elseif ($UpdateBaseline) { Set-Result 'd3d12 log' 'PASS' $detail; $logOk = $true }
    elseif ($null -eq $baseLive) { Set-Result 'd3d12 log' 'FAIL' "$detail; no $baseLiveFile" }
    elseif ($liveLines.Count -gt $baseLive) { Set-Result 'd3d12 log' 'FAIL' $detail; Show-Lines $liveLines 10 }
    else { Set-Result 'd3d12 log' 'PASS' $detail; $logOk = $true }
}

# ---------------------------------------------------------------- 6. tests
if ($NoTests) {
    Set-Result 'tests' 'SKIP' '-NoTests'
}
elseif (Test-Path -LiteralPath $exe) {
    foreach ($f in @($testsFile, $testsLog)) { if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Force } }
    $run = Invoke-Engine @('--run-tests', $testsFile, '--verify-log', $testsLog)
    $lines = if (Test-Path -LiteralPath $testsFile) { @([System.IO.File]::ReadAllLines($testsFile)) } else { $null }
    $testsBody = Read-LogBody $testsLog
    $testsD3d = @($testsBody | Where-Object { $_ -cmatch '^D3D12 ' })
    if ($run.TimedOut) { Set-Result 'tests' 'FAIL' "hung; killed after $ProcessTimeoutSec s"; Show-Lines (Select-Stderr $testsBody) 10 }
    elseif ($null -eq $lines) { Set-Result 'tests' 'FAIL' "exit $(Format-ExitCode $run.Code); no $testsFile"; Show-Lines (Select-Stderr $testsBody) 10 }
    else {
        $passed = @($lines | Where-Object { $_ -cmatch '^PASS ' } | ForEach-Object { $_.Substring(5) })
        $failed = @($lines | Where-Object { $_ -cmatch '^FAIL ' })
        $lost = @()
        $baseTests = Join-Path $Baseline 'tests.txt'
        if (-not $UpdateBaseline -and (Test-Path -LiteralPath $baseTests)) {
            $lost = @([System.IO.File]::ReadAllLines($baseTests) | Where-Object { $_ -cmatch '^PASS ' } |
                      ForEach-Object { $_.Substring(5) } | Where-Object { $passed -cnotcontains $_ })
        }
        $detail = "$($passed.Count) passed, $($failed.Count) failed, exit $(Format-ExitCode $run.Code)"
        if ($lost.Count) { $detail += ", $($lost.Count) baseline test(s) no longer pass" }
        if (-not (Test-Path -LiteralPath $testsLog)) { $detail += "; no $testsLog" }
        elseif ($testsD3d.Count) { $detail += "; $($testsD3d.Count) D3D12 message(s)" }
        if ($failed.Count -or $run.Code -ne 0 -or $lost.Count -or $passed.Count -eq 0 -or $testsD3d.Count -or
            -not (Test-Path -LiteralPath $testsLog)) {
            Set-Result 'tests' 'FAIL' $detail
            Show-Lines (@($failed) + @($lost | ForEach-Object { "not passing: $_" }) + @(Select-Stderr $testsBody) + $testsD3d) 10
        }
        elseif (-not $UpdateBaseline -and -not (Test-Path -LiteralPath $baseTests)) { Set-Result 'tests' 'FAIL' "$detail; no $baseTests" }
        else { Set-Result 'tests' 'PASS' $detail }
    }
}
else {
    Set-Result 'tests' 'FAIL' "no $exe"
}

# ---------------------------------------------------------------- summary
$failures = @($results | Where-Object { $_.Status -eq 'FAIL' })
Write-Output ''
Write-Output "verify summary (tier $Tier$(if ($Poison) { ", poison '$Poison'" })); logs in $work"
foreach ($r in $results) { Write-Output ('  {0,-14} {1,-7} {2}' -f $r.Step, $r.Status, $r.Detail) }

# ---------------------------------------------------------------- baseline update
if ($UpdateBaseline) {
    if ($failures.Count -or -not $sweepOk -or -not $logOk) {
        Write-Output 'verify: FAIL (the run is not clean; the baseline was not touched)'
        exit 1
    }
    New-Item -ItemType Directory -Force -Path $baseTier | Out-Null
    Get-ChildItem -LiteralPath $baseTier -File | Where-Object { $_.Name -match '\.(bmp|fg\.txt|timers\.txt)$' -or $_.Name -eq 'd3d12.log' } |
        Remove-Item -Force
    Get-ChildItem -LiteralPath $sweepDir -File | Where-Object { $_.Name -match '\.(bmp|fg\.txt|timers\.txt)$' -or $_.Name -eq 'd3d12.log' } |
        Copy-Item -Destination $baseTier
    $header = "# live-object lines of a clean $Tier run, recorded by verify.ps1 -UpdateBaseline on $(Get-Date -Format 'yyyy-MM-dd HH:mm')"
    [System.IO.File]::WriteAllLines((Join-Path $Baseline 'live_objects.txt'), [string[]](@($header) + @($liveLines)))
    $written = "$baseTier, live_objects.txt"
    if (-not $NoTests) { Copy-Item -LiteralPath $testsFile -Destination (Join-Path $Baseline 'tests.txt') -Force; $written += ', tests.txt' }
    Write-Output "verify: BASELINE UPDATED ($written). Add a dated line to $(Join-Path $Baseline 'CHANGELOG.md')."
    exit 0
}

if ($failures.Count) {
    Write-Output "verify: FAIL ($(($failures | ForEach-Object Step) -join ', '))"
    exit 1
}
Write-Output 'verify: PASS'
exit 0
