<#
.SYNOPSIS
    Layering and API-ownership audit of the engine sources (rules A1-A6 and A8-A13).

.DESCRIPTION
    audit.ps1 [-Report]

    Scans *.h, *.hpp and *.cpp under Source\, and fails if that directory is missing.
    Every rule matches code: // and /* */ comments are stripped first (string literals are kept),
    and every pattern matches case-sensitively.

    Default mode prints one count line per rule plus every enforced violation, as
    '<file>(<line>): error <rule>: <text>', and exits 1 if there is any. MC_Engine.props runs it in
    this mode as the PreBuildEvent of both configurations, so an enforced violation fails the build.
    -Report also lists every hit as file:line and always exits 0.

    Exemptions: audit_allow.txt holds '<rule> <glob>' lines. The glob is matched against the path
    relative to Source\ (backslash separators): '*' stays inside one directory, '**' crosses
    directories. 'A9 <number>' stores the Legacy\ count.

    Enforcement: a non-exempt hit fails the audit only if $Enforced below holds '<rule> <glob>' with a
    glob that matches the file (same matching as audit_allow.txt). Rules are switched on here, one
    layer at a time, as the refactor reaches them; everything else is reported only.
#>
param([switch]$Report)

$ErrorActionPreference = 'Stop'

# Enforced rules: '<rule> <glob>'. A rule with no line here is report-only.
$Enforced = @(
    'A2 Core\**'
    'A2 Platform\**'
    'A2 Renderer\*.h'
    'A2 Renderer\D3D12\*'
    'A3 Core\**'
    'A3 Platform\**'
    'A3 Renderer\*.h'
    'A3 Renderer\D3D12\*'
    'A5 Core\**'
    'A5 Platform\**'
    'A5 Renderer\*.h'
    'A5 Renderer\D3D12\*'
    'A9 Legacy'
    'A10 **'
    'A11 **'
)

$engineDir = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path.TrimEnd('\')
$sourceDir = Join-Path $engineDir 'Source'
if (-not (Test-Path -LiteralPath $sourceDir -PathType Container)) {
    Write-Output "audit: no Source\ directory under $engineDir -> FAIL"
    exit 1
}
$root = $sourceDir

# ---------------------------------------------------------------- path matching
# '*' and '?' match within one path segment, '**' matches across segments; case-insensitive.
function Test-Glob([string]$rel, [string]$glob) {
    $rx = [regex]::Escape($glob).Replace('\*\*', '.*').Replace('\*', '[^\\]*').Replace('\?', '[^\\]')
    return $rel -match "^$rx$"
}

function Read-RuleLines([string[]]$lines, [string]$origin) {
    foreach ($line in $lines) {
        $t = $line.Trim()
        if (-not $t -or $t.StartsWith('#')) { continue }
        $rule, $glob = $t -split '\s+', 2
        if (-not $glob) { Write-Host "audit: ignoring malformed $origin line '$t'"; continue }
        [pscustomobject]@{ Rule = $rule; Glob = $glob }
    }
}

$allowPath = Join-Path $PSScriptRoot 'audit_allow.txt'
$allowAll = @(if (Test-Path -LiteralPath $allowPath) { Read-RuleLines ([System.IO.File]::ReadAllLines($allowPath)) 'audit_allow.txt' })
$allow = @($allowAll | Where-Object { -not ($_.Rule -eq 'A9' -and $_.Glob -match '^\d+$') })
$storedLegacy = @($allowAll | Where-Object { $_.Rule -eq 'A9' -and $_.Glob -match '^\d+$' } | ForEach-Object { [int]$_.Glob })
$enforce = @(Read-RuleLines $Enforced 'enforced-rule')

function Get-Allowed([string]$rule, [string]$rel) {
    foreach ($a in $allow) { if ($a.Rule -eq $rule -and (Test-Glob $rel $a.Glob)) { return $a.Glob } }
    return $null
}
function Test-Enforced([string]$rule, [string]$rel) {
    foreach ($e in $enforce) { if ($e.Rule -eq $rule -and (Test-Glob $rel $e.Glob)) { return $true } }
    return $false
}
function Test-Home([string]$rel, [string[]]$homes) {
    foreach ($h in $homes) { if (Test-Glob $rel $h) { return $true } }
    return $false
}

# Layer of a path relative to Source\ (also used for root-relative include strings).
$LayerNames = @('Core', 'Platform', 'Renderer', 'Assets', 'Scene', 'Engine', 'Editor', 'App', 'Legacy')
function Get-Layer([string]$rel) {
    $p = $rel.Replace('/', '\')
    if ($p -like 'Renderer\D3D12\*') { return 'Backend' }
    $first = ($p -split '\\', 2)[0]
    foreach ($l in $LayerNames) { if ($first -eq $l -and $p.Contains('\')) { return $l } }
    return $null
}
function Test-InBackend([string]$rel) { $rel -like 'Renderer\D3D12\*' }

# May-include table: including layer -> layers (and single headers) it may include.
$MayInclude = @{
    Core     = @('Core')
    Platform = @('Core', 'Platform')
    Renderer = @('Core', 'Renderer')
    Backend  = @('Core', 'Renderer', 'Backend')
    Assets   = @('Core', 'Assets', 'Renderer\RenderTypes.h')
    Scene    = @('Core', 'Assets', 'Scene', 'Renderer\RenderTypes.h', 'Platform\Input.h')
    Engine   = @('Core', 'Platform', 'Renderer', 'Engine')
    Editor   = @('Core', 'Platform', 'Renderer', 'Assets', 'Scene', 'Editor')
    App      = @('Core', 'Platform', 'Renderer', 'Assets', 'Scene', 'Engine', 'Editor', 'App')
}

# ---------------------------------------------------------------- rules
# Kind 'site': every hit is listed. Kind 'file': hits are listed once per file with a count.
# Forbidden: scriptblock over $rel; true where the pattern must not appear.
$Rules = @(
    @{ Id = 'A1'; Kind = 'site'; Title = 'ResourceBarrier( outside BarrierManager'
       Pattern = 'ResourceBarrier\s*\('; Forbidden = { param($rel) $true } }
    @{ Id = 'A2'; Kind = 'file'; Title = 'D3D12/DXGI include or identifier outside Renderer\D3D12\'
       Pattern = '#include\s*[<"][^>"]*(d3d12|dxgi|d3dx12|dxcapi|imgui_impl_dx12|DDSTextureLoader|SharedTypes\.h)|\b(ID3D12\w*|IDXGI\w*|D3D12_\w+|DXGI_\w+)\b'
       Forbidden = { param($rel) -not (Test-InBackend $rel) } }
    @{ Id = 'A3'; Kind = 'site'; Title = '#include "Renderer/D3D12/..." outside Renderer\D3D12\'
       Pattern = '#include\s*"Renderer[/\\]D3D12[/\\]'; Forbidden = { param($rel) -not (Test-InBackend $rel) } }
    @{ Id = 'A4'; Kind = 'file'; Title = 'nlohmann outside Core\Stream.cpp and Core\JsonMigrator.*'
       Pattern = 'nlohmann'
       Forbidden = { param($rel) -not (Test-Home $rel @('Core\Stream.cpp', 'Core\JsonMigrator.cpp', 'Core\JsonMigrator.h')) } }
    @{ Id = 'A6'; Kind = 'site'; Title = 'Singleton<, GetLib(), GetPicker(, static D3DApp'
       Pattern = 'Singleton<|GetLib\(\)|GetPicker\(|static D3DApp\b'; Forbidden = { param($rel) $true } }
    @{ Id = 'A8a'; Kind = 'site'; Title = 'gNumFrameResources outside Renderer\D3D12\'
       Pattern = '\bgNumFrameResources\b'; Forbidden = { param($rel) -not (Test-InBackend $rel) } }
    @{ Id = 'A8b'; Kind = 'file'; Title = 'ImGui include or ImGui::/ImGui_Impl outside Renderer\D3D12\ and Editor\'
       Pattern = '#include\s*[<"](?:[^>"]*[/\\])?(?:imgui[^>"/\\]*|imconfig\.h|imstb[^>"/\\]*|ImGuizmo[^>"/\\]*)[>"]|\bImGui::|\bImGui_Impl'
       Forbidden = { param($rel) -not ((Test-InBackend $rel) -or ($rel -like 'Editor\*')) } }
    @{ Id = 'A11'; Kind = 'site'; Title = 'm_currState outside BarrierManager.* and MC_Types.h'
       Pattern = '\bm_currState\b'
       Forbidden = { param($rel) -not (Test-Home $rel @('Renderer\D3D12\BarrierManager.*', 'Renderer\D3D12\MC_Types.h')) } }
    @{ Id = 'A12'; Kind = 'site'; Title = 'Create*View( outside DescHeapManager.cpp'
       Pattern = 'Create(ShaderResource|UnorderedAccess|ConstantBuffer|RenderTarget|DepthStencil)View\s*\('
       Forbidden = { param($rel) -not (Test-Home $rel @('Renderer\D3D12\DescHeapManager.cpp')) } }
    @{ Id = 'A13'; Kind = 'site'; Title = 'SetDescriptorHeaps( outside DescHeapManager.cpp'
       Pattern = 'SetDescriptorHeaps\s*\('
       Forbidden = { param($rel) -not (Test-Home $rel @('Renderer\D3D12\DescHeapManager.cpp')) } }
)
foreach ($r in $Rules) { $r.Regex = New-Object System.Text.RegularExpressions.Regex($r.Pattern) }   # case-sensitive

$A5Title  = 'include direction between layers; <windows.h> in Core headers'
$A9Title  = 'Legacy\ file count must not grow'
$A10Title = 'no two files under Source\ share a basename'
$RuleOrder = @('A1', 'A2', 'A3', 'A4', 'A5', 'A6', 'A8a', 'A8b', 'A9', 'A10', 'A11', 'A12', 'A13')
$Titles = @{ A5 = $A5Title; A9 = $A9Title; A10 = $A10Title }
foreach ($r in $Rules) { $Titles[$r.Id] = $r.Title }
$KindOf = @{ A5 = 'site'; A9 = 'site'; A10 = 'site' }
foreach ($r in $Rules) { $KindOf[$r.Id] = $r.Kind }

# ---------------------------------------------------------------- scan
# Comments become blank (newlines kept, so line numbers hold); string, char and raw-string
# literals are matched first so comment markers inside them are left alone.
$tokenRegex = New-Object System.Text.RegularExpressions.Regex(
    'R"([^()\\\s]{0,16})\((?s:.*?)\)\1"|"(?:\\.|[^"\\\r\n])*"|''(?:\\.|[^''\\\r\n])*''|//[^\r\n]*|/\*(?s:.*?)\*/')
$stripComment = [System.Text.RegularExpressions.MatchEvaluator] {
    param($m)
    if ($m.Value[0] -eq [char]'/') { return ($m.Value -replace '[^\n]', '') }
    return $m.Value
}
$includeRegex = New-Object System.Text.RegularExpressions.Regex('#include\s*"([^"]+)"')
$windowsRegex = New-Object System.Text.RegularExpressions.Regex('#include\s*[<"](?i:windows\.h)[>"]')

$files = @(Get-ChildItem -LiteralPath $sourceDir -Recurse -File -Include *.h, *.hpp, *.cpp | Sort-Object FullName -Unique)

$hits = New-Object System.Collections.Generic.List[object]
function Add-Hit([string]$rule, [string]$rel, [int]$line, [string]$text) {
    $allowedBy = Get-Allowed $rule $rel
    $hits.Add([pscustomobject]@{
        Rule = $rule; Rel = $rel; Line = $line; Text = $text.Trim()
        Allowed = $allowedBy; Enforced = (-not $allowedBy) -and (Test-Enforced $rule $rel)
    })
}

foreach ($f in $files) {
    $rel = $f.FullName.Substring($root.Length + 1)
    $code = $tokenRegex.Replace([System.IO.File]::ReadAllText($f.FullName), $stripComment)
    $lineStarts = New-Object System.Collections.Generic.List[int]
    $lineStarts.Add(0)
    for ($i = $code.IndexOf("`n"); $i -ge 0; $i = $code.IndexOf("`n", $i + 1)) { $lineStarts.Add($i + 1) }
    $getLine = {
        param([int]$index)
        $n = $lineStarts.BinarySearch($index)
        if ($n -lt 0) { $n = (-bnot $n) - 1 }
        $end = $code.IndexOf("`n", $lineStarts[$n])
        if ($end -lt 0) { $end = $code.Length }
        , @(($n + 1), $code.Substring($lineStarts[$n], $end - $lineStarts[$n]))
    }

    foreach ($r in $Rules) {
        if (-not (& $r.Forbidden $rel)) { continue }
        foreach ($m in $r.Regex.Matches($code)) {
            $ln = & $getLine $m.Index
            Add-Hit $r.Id $rel $ln[0] $ln[1]
        }
    }

    # A5
    $from = Get-Layer $rel
    if ($from -and $from -ne 'Legacy') {
        foreach ($m in $includeRegex.Matches($code)) {
            $target = $m.Groups[1].Value.Replace('/', '\')
            $to = Get-Layer $target
            if (-not $to -or $to -eq $from) { continue }
            $ok = $false
            foreach ($may in $MayInclude[$from]) {
                if ($may.Contains('\')) { if ($target -eq $may) { $ok = $true } }
                elseif ($may -eq $to) { $ok = $true }
            }
            if (-not $ok) { $ln = & $getLine $m.Index; Add-Hit 'A5' $rel $ln[0] ("$from -> $to`: " + $ln[1].Trim()) }
        }
        if ($from -eq 'Core' -and $rel -match '\.(h|hpp)$') {
            foreach ($m in $windowsRegex.Matches($code)) { $ln = & $getLine $m.Index; Add-Hit 'A5' $rel $ln[0] $ln[1] }
        }
    }
}

# A9
$legacyDir = Join-Path $sourceDir 'Legacy'
$legacyCount = @(if (Test-Path -LiteralPath $legacyDir) { Get-ChildItem -LiteralPath $legacyDir -Recurse -File }).Count
$stored = if ($storedLegacy.Count) { $storedLegacy[-1] } else { $null }
if ($null -eq $stored -or $legacyCount -gt $stored) {
    $why = if ($null -eq $stored) { "no 'A9 <count>' line in audit_allow.txt" } else { "grew from $stored" }
    $hits.Add([pscustomobject]@{ Rule = 'A9'; Rel = 'Legacy'; Line = 0; Text = "$legacyCount files, $why"
                                 Allowed = $null; Enforced = (Test-Enforced 'A9' 'Legacy') })
}
# A10
$all = Get-ChildItem -LiteralPath $sourceDir -Recurse -File
foreach ($g in ($all | Group-Object { $_.Name.ToLowerInvariant() } | Where-Object Count -gt 1)) {
    $paths = @($g.Group | ForEach-Object { $_.FullName.Substring($sourceDir.Length + 1) })
    $hits.Add([pscustomobject]@{ Rule = 'A10'; Rel = $paths[0]; Line = 0; Text = ($paths -join ', ')
                                 Allowed = $null; Enforced = (Test-Enforced 'A10' $paths[0]) })
}

# ---------------------------------------------------------------- output
Write-Output "audit: $($files.Count) files under $root"
$violations = @($hits | Where-Object Enforced)
foreach ($id in $RuleOrder) {
    $label = '{0,-4} {1,-72}' -f $id, $Titles[$id]
    $mine = @($hits | Where-Object Rule -eq $id | Sort-Object Rel, Line)
    $allowedCount = @($mine | Where-Object Allowed).Count
    $open = $mine.Count - $allowedCount
    $state = if (@($enforce | Where-Object Rule -eq $id).Count) { 'enforced' } else { 'report' }
    $extra = if ($id -eq 'A9') { " (Legacy files: $legacyCount, stored: $(if ($storedLegacy.Count) { $storedLegacy[-1] } else { 'none' }))" } else { '' }
    $filesHit = @($mine | Where-Object { -not $_.Allowed } | Select-Object -ExpandProperty Rel -Unique).Count
    Write-Output ($label + (" {0,5} hit(s) in {1,3} file(s), {2} allowed [{3}]" -f $open, $filesHit, $allowedCount, $state) + $extra)
    if (-not $Report) { continue }
    if ($KindOf[$id] -eq 'file') {
        foreach ($g in ($mine | Group-Object Rel)) {
            $first = $g.Group[0]
            $tag = if ($first.Allowed) { " [allowed: $($first.Allowed)]" } elseif ($first.Enforced) { ' [ENFORCED]' } else { '' }
            $more = if ($g.Count -gt 1) { " (+$($g.Count - 1) more)" } else { '' }
            Write-Output "      $($first.Rel):$($first.Line)$more$tag"
        }
    }
    else {
        foreach ($h in $mine) {
            $tag = if ($h.Allowed) { " [allowed: $($h.Allowed)]" } elseif ($h.Enforced) { ' [ENFORCED]' } else { '' }
            $loc = if ($h.Line -gt 0) { "$($h.Rel):$($h.Line)" } else { $h.Rel }
            $text = if ($h.Text.Length -gt 110) { $h.Text.Substring(0, 110) + '...' } else { $h.Text }
            Write-Output "      $loc$tag  $text"
        }
    }
}

if (-not $Report) {
    foreach ($v in $violations) {
        $path = if ($v.Line -gt 0) { "$(Join-Path $root $v.Rel)($($v.Line))" } else { Join-Path $root $v.Rel }
        Write-Output "$path`: error $($v.Rule): $($Titles[$v.Rule]): $($v.Text)"
    }
}
Write-Output ("audit: {0} enforced violation(s) -> {1}" -f $violations.Count, $(if ($violations.Count) { 'FAIL' } else { 'PASS' }))
if (-not $Report -and $violations.Count) { exit 1 }
exit 0
