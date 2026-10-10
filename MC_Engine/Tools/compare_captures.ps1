<#
.SYNOPSIS
    Per-pixel comparison of capture-sweep BMPs, and the nondeterminism mask they are judged with.

.DESCRIPTION
    Compare:
        compare_captures.ps1 [-Baseline] <dir> [-Current] <dir> [-Mask <file>]
    Compares every <name>.bmp in the baseline directory with the same-named file in the current
    directory. A pixel differs when any of its four BGRA bytes differ; pixels the mask lists for
    that file are ignored. Prints the differing-pixel count per file and in total, and exits 0 only
    if every count is 0 (or within that file's scatter budget) and no baseline file is missing from
    the current directory.

    Build the mask:
        compare_captures.ps1 -BuildMask -Runs <dir1>,<dir2>,... -Out <file>
    Marks, per file name, every pixel that differs between any of the runs. Every run must hold
    the same set of BMPs with the same sizes.

    Mask file: a text file with one line per capture that has masked pixels,
        <name>.bmp<TAB><width><TAB><height><TAB><start>+<length> <start>+<length> ...
    where each run covers <length> consecutive pixel indices y*width+x, y counted from the top row.
    Lines starting with '#' are comments. A name may appear on several lines and their pixels are
    combined, so a mask built later from new runs can be appended to an existing one.
    A capture whose nondeterministic pixels land in new places in every run can also have one
    scatter-budget line, written by hand:
        <name>.bmp<TAB>scatter<TAB><n>
    Up to <n> differing pixels outside the mask then pass for that file; they are counted and
    reported separately. -BuildMask never writes these lines.

    Reads the BMP bytes directly (BITMAPFILEHEADER + BITMAPINFOHEADER, 24 or 32 bpp, BI_RGB,
    either row order), so a full sweep compares in a few seconds.
#>
[CmdletBinding(DefaultParameterSetName = 'Compare')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Compare', Position = 0)] [string]$Baseline,
    [Parameter(Mandatory = $true, ParameterSetName = 'Compare', Position = 1)] [string]$Current,
    [Parameter(ParameterSetName = 'Compare')] [string]$Mask,
    [Parameter(Mandatory = $true, ParameterSetName = 'BuildMask')] [switch]$BuildMask,
    [Parameter(Mandatory = $true, ParameterSetName = 'BuildMask')] [string[]]$Runs,
    [Parameter(Mandatory = $true, ParameterSetName = 'BuildMask')] [string]$Out
)

$ErrorActionPreference = 'Stop'

if (-not ('MCCaptureCompare' -as [type])) {
    Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

public static class MCCaptureCompare
{
    public sealed class Image
    {
        public int Width;
        public int Height;
        public byte[] Pixels;   // top-down rows, 4 bytes per pixel (B, G, R, A)
    }

    public sealed class DiffResult
    {
        public long Differing;  // differing pixels outside the mask
        public long Masked;     // differing pixels the mask hid
        public int MinX = int.MaxValue, MinY = int.MaxValue, MaxX = -1, MaxY = -1;
    }

    public static Image Load(string path)
    {
        byte[] f = File.ReadAllBytes(path);
        if (f.Length < 54 || f[0] != (byte)'B' || f[1] != (byte)'M')
            throw new InvalidDataException("not a BMP file");
        int offBits = BitConverter.ToInt32(f, 10);
        int width = BitConverter.ToInt32(f, 18);
        int height = BitConverter.ToInt32(f, 22);
        int bpp = BitConverter.ToUInt16(f, 28);
        int compression = BitConverter.ToInt32(f, 30);
        if (compression != 0 || (bpp != 24 && bpp != 32))
            throw new InvalidDataException("unsupported BMP (" + bpp + " bpp, compression " + compression + ")");
        bool bottomUp = height > 0;
        height = Math.Abs(height);
        if (width <= 0 || height == 0)
            throw new InvalidDataException("empty BMP");
        int bytesPerPixel = bpp / 8;
        long stride = ((long)width * bpp + 31) / 32 * 4;
        if (offBits < 0 || offBits + stride * height > f.Length)
            throw new InvalidDataException("truncated BMP");

        var image = new Image { Width = width, Height = height, Pixels = new byte[(long)width * height * 4] };
        for (int y = 0; y < height; ++y)
        {
            long src = offBits + stride * (bottomUp ? height - 1 - y : y);
            long dst = (long)y * width * 4;
            for (int x = 0; x < width; ++x, src += bytesPerPixel, dst += 4)
            {
                image.Pixels[dst + 0] = f[src + 0];
                image.Pixels[dst + 1] = f[src + 1];
                image.Pixels[dst + 2] = f[src + 2];
                image.Pixels[dst + 3] = bytesPerPixel == 4 ? f[src + 3] : (byte)255;
            }
        }
        return image;
    }

    static bool PixelDiffers(byte[] a, byte[] b, long i)
    {
        long o = i * 4;
        return a[o] != b[o] || a[o + 1] != b[o + 1] || a[o + 2] != b[o + 2] || a[o + 3] != b[o + 3];
    }

    // mask may be null.
    public static DiffResult Diff(Image a, Image b, bool[] mask)
    {
        var r = new DiffResult();
        long count = (long)a.Width * a.Height;
        for (long i = 0; i < count; ++i)
        {
            if (!PixelDiffers(a.Pixels, b.Pixels, i)) continue;
            if (mask != null && mask[i]) { ++r.Masked; continue; }
            ++r.Differing;
            int x = (int)(i % a.Width), y = (int)(i / a.Width);
            if (x < r.MinX) r.MinX = x;
            if (x > r.MaxX) r.MaxX = x;
            if (y < r.MinY) r.MinY = y;
            if (y > r.MaxY) r.MaxY = y;
        }
        return r;
    }

    // Marks into mask every pixel where b differs from a.
    public static void Accumulate(Image a, Image b, bool[] mask)
    {
        long count = (long)a.Width * a.Height;
        for (long i = 0; i < count; ++i)
            if (PixelDiffers(a.Pixels, b.Pixels, i)) mask[i] = true;
    }

    public static long CountSet(bool[] mask)
    {
        long n = 0;
        foreach (bool m in mask) if (m) ++n;
        return n;
    }

    public static string EncodeRuns(bool[] mask)
    {
        var sb = new StringBuilder();
        for (long i = 0; i < mask.LongLength; ++i)
        {
            if (!mask[i]) continue;
            long start = i;
            while (i + 1 < mask.LongLength && mask[i + 1]) ++i;
            if (sb.Length > 0) sb.Append(' ');
            sb.Append(start.ToString(CultureInfo.InvariantCulture)).Append('+')
              .Append((i - start + 1).ToString(CultureInfo.InvariantCulture));
        }
        return sb.ToString();
    }

    // Adds the runs to mask (allocated when null) and returns it.
    public static bool[] DecodeRuns(string runs, int width, int height, bool[] mask)
    {
        if (mask == null) mask = new bool[(long)width * height];
        foreach (string run in runs.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries))
        {
            string[] p = run.Split('+');
            long start = long.Parse(p[0], CultureInfo.InvariantCulture);
            long length = long.Parse(p[1], CultureInfo.InvariantCulture);
            if (start < 0 || length < 0 || start + length > mask.LongLength)
                throw new InvalidDataException("mask run " + run + " is outside a " + width + "x" + height + " image");
            for (long i = start; i < start + length; ++i) mask[i] = true;
        }
        return mask;
    }
}
'@
}

function Get-BmpNames([string]$dir) {
    @(Get-ChildItem -LiteralPath $dir -Filter '*.bmp' -File | Sort-Object Name | ForEach-Object { $_.Name })
}

function Format-Box($r) {
    "x $($r.MinX)..$($r.MaxX), y $($r.MinY)..$($r.MaxY) from the top"
}

# ---------------------------------------------------------------- build the mask
if ($BuildMask) {
    $runDirs = @($Runs | ForEach-Object { $_ -split ',' } | Where-Object { $_.Trim() } | ForEach-Object { $_.Trim() })
    if ($runDirs.Count -lt 2) { Write-Output 'compare_captures: -BuildMask needs at least two run directories'; exit 1 }
    foreach ($d in $runDirs) {
        if (-not (Test-Path -LiteralPath $d -PathType Container)) { Write-Output "compare_captures: no such run directory: $d"; exit 1 }
    }
    $names = Get-BmpNames $runDirs[0]
    if ($names.Count -eq 0) { Write-Output "compare_captures: no BMPs in $($runDirs[0])"; exit 1 }
    $bad = $false
    foreach ($d in $runDirs[1..($runDirs.Count - 1)]) {
        $other = Get-BmpNames $d
        foreach ($n in $names) { if ($other -notcontains $n) { Write-Output "compare_captures: $n missing in $d"; $bad = $true } }
        foreach ($n in $other) { if ($names -notcontains $n) { Write-Output "compare_captures: $n only in $d"; $bad = $true } }
    }
    if ($bad) { exit 1 }

    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add('# Capture nondeterminism mask: pixels that differed between runs.')
    $lines.Add("# Built $(Get-Date -Format 'yyyy-MM-dd HH:mm') from: $($runDirs -join ', ')")
    $lines.Add('# <name>.bmp<TAB><width><TAB><height><TAB><start>+<length> ... (pixel index y*width+x, y from the top)')
    [long]$total = 0
    foreach ($n in $names) {
        try {
            $first = [MCCaptureCompare]::Load((Join-Path $runDirs[0] $n))
            $maskBits = New-Object 'bool[]' ([long]$first.Width * $first.Height)
            foreach ($d in $runDirs[1..($runDirs.Count - 1)]) {
                $img = [MCCaptureCompare]::Load((Join-Path $d $n))
                if ($img.Width -ne $first.Width -or $img.Height -ne $first.Height) {
                    throw "size $($img.Width)x$($img.Height) in $d, $($first.Width)x$($first.Height) in $($runDirs[0])"
                }
                [MCCaptureCompare]::Accumulate($first, $img, $maskBits)
            }
        }
        catch {
            Write-Output ("compare_captures: {0}: {1}" -f $n, $_.Exception.Message)
            exit 1
        }
        $set = [MCCaptureCompare]::CountSet($maskBits)
        $total += $set
        Write-Output ('  {0,-36} {1,8} masked' -f $n, $set)
        if ($set -gt 0) {
            $lines.Add("$n`t$($first.Width)`t$($first.Height)`t$([MCCaptureCompare]::EncodeRuns($maskBits))")
        }
    }
    $outDir = Split-Path -Parent $Out
    if ($outDir -and -not (Test-Path -LiteralPath $outDir)) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }
    [System.IO.File]::WriteAllLines($Out, $lines, (New-Object System.Text.UTF8Encoding($false)))
    Write-Output "compare_captures: mask of $total pixel(s) over $($names.Count) file(s) from $($runDirs.Count) runs -> $Out"
    exit 0
}

# ---------------------------------------------------------------- compare
foreach ($d in @($Baseline, $Current)) {
    if (-not (Test-Path -LiteralPath $d -PathType Container)) { Write-Output "compare_captures: no such directory: $d"; exit 1 }
}
$baseNames = Get-BmpNames $Baseline
if ($baseNames.Count -eq 0) { Write-Output "compare_captures: no BMPs in the baseline $Baseline"; exit 1 }
$curNames = Get-BmpNames $Current

# name -> list of @{ Width; Height; Runs }; several lines for one name are combined
$maskEntries = @{}
$budgets = @{}   # name -> scatter budget (differing pixels outside the mask that still pass)
if ($Mask) {
    if (-not (Test-Path -LiteralPath $Mask -PathType Leaf)) { Write-Output "compare_captures: no such mask file: $Mask"; exit 1 }
    foreach ($line in [System.IO.File]::ReadAllLines((Resolve-Path -LiteralPath $Mask).Path)) {
        if (-not $line.Trim() -or $line.StartsWith('#')) { continue }
        $f = $line -split "`t"
        if ($f.Count -eq 3 -and $f[1] -ceq 'scatter' -and $f[2] -match '^\d+$' -and -not $budgets.ContainsKey($f[0])) {
            $budgets[$f[0]] = [long]$f[2]
            continue
        }
        if ($f.Count -lt 4) { Write-Output "compare_captures: malformed mask line: $($line.Substring(0, [Math]::Min(80, $line.Length)))"; exit 1 }
        if (-not $maskEntries.ContainsKey($f[0])) { $maskEntries[$f[0]] = New-Object System.Collections.Generic.List[object] }
        $maskEntries[$f[0]].Add(@{ Width = [int]$f[1]; Height = [int]$f[2]; Runs = $f[3] })
    }
}

[long]$total = 0
[long]$totalMasked = 0
[long]$totalScatter = 0
$missing = 0
$errors = 0
Write-Output "compare_captures: $Current vs $Baseline$(if ($Mask) { " (mask $Mask)" })"
foreach ($n in $baseNames) {
    if ($curNames -notcontains $n) {
        Write-Output ('  {0,-36} MISSING' -f $n)
        $missing++
        continue
    }
    try {
        $a = [MCCaptureCompare]::Load((Join-Path $Baseline $n))
        $b = [MCCaptureCompare]::Load((Join-Path $Current $n))
        if ($a.Width -ne $b.Width -or $a.Height -ne $b.Height) {
            $px = [Math]::Max([long]$a.Width * $a.Height, [long]$b.Width * $b.Height)
            Write-Output ('  {0,-36} {1,8} differing (size {2}x{3}, baseline {4}x{5})' -f $n, $px, $b.Width, $b.Height, $a.Width, $a.Height)
            $total += $px
            continue
        }
        $bits = $null
        $note = ''
        if ($maskEntries.ContainsKey($n)) {
            foreach ($e in $maskEntries[$n]) {
                if ($e.Width -eq $a.Width -and $e.Height -eq $a.Height) {
                    $bits = [MCCaptureCompare]::DecodeRuns($e.Runs, $e.Width, $e.Height, $bits)
                }
                else {
                    $note = "  (a mask line is $($e.Width)x$($e.Height), not applied)"
                }
            }
        }
        $r = [MCCaptureCompare]::Diff($a, $b, $bits)
        $totalMasked += $r.Masked
        if ($r.Differing -gt 0 -and $budgets.ContainsKey($n) -and $r.Differing -le $budgets[$n]) {
            $totalScatter += $r.Differing
            $note += "  (within the scatter budget of $($budgets[$n]))"
        }
        else {
            $total += $r.Differing
        }
        $where = if ($r.Differing -gt 0) { "  $(Format-Box $r)" } else { '' }
        Write-Output ('  {0,-36} {1,8} differing {2,8} masked{3}{4}' -f $n, $r.Differing, $r.Masked, $where, $note)
    }
    catch {
        Write-Output ('  {0,-36} ERROR {1}' -f $n, $_.Exception.Message)
        $errors++
    }
}
foreach ($n in $curNames) {
    if ($baseNames -notcontains $n) { Write-Output ('  {0,-36} not in the baseline (not compared)' -f $n) }
}

$ok = ($total -eq 0 -and $missing -eq 0 -and $errors -eq 0)
Write-Output ("compare_captures: {0} differing pixel(s) outside the mask, {1} masked, {2} within scatter budgets, {3} file(s), {4} missing, {5} unreadable -> {6}" -f `
    $total, $totalMasked, $totalScatter, $baseNames.Count, $missing, $errors, $(if ($ok) { 'PASS' } else { 'FAIL' }))
if ($ok) { exit 0 } else { exit 1 }
