# Zip windows-mcp-delta's source (pyproject.toml, README.md, src/) into the
# path given, for app.rc to embed. Tests, caches and build leftovers are left
# out. Entry names use forward slashes so the archive reads the same on every
# PowerShell version, in the program's ZIP reader and in uvx.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\pack-delta.ps1 <out.zip>
param([Parameter(Mandatory=$true)][string]$Out)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null

$root = Join-Path (Split-Path -Parent $PSScriptRoot) 'windows-mcp-delta'
$want = @()
foreach ($top in @('pyproject.toml', 'README.md')) {
    $p = Join-Path $root $top
    if (Test-Path $p) { $want += [pscustomobject]@{ Path = $p; Name = $top } }
}
foreach ($f in Get-ChildItem -Recurse -File (Join-Path $root 'src')) {
    $rel = $f.FullName.Substring($root.Length + 1).Replace('\', '/')
    if ($rel -match '/__pycache__/' -or $rel -match '\.pyc$') { continue }
    $want += [pscustomobject]@{ Path = $f.FullName; Name = $rel }
}

$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
if (Test-Path $Out) { Remove-Item -Force $Out }

$zip = [System.IO.Compression.ZipFile]::Open($Out, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($item in $want) {
        $entry = $zip.CreateEntry($item.Name, [System.IO.Compression.CompressionLevel]::Optimal)
        $in = [System.IO.File]::OpenRead($item.Path)
        try {
            $stream = $entry.Open()   # not $out: PowerShell's names ignore case, and $Out is the path
            try { $in.CopyTo($stream) } finally { $stream.Dispose() }
        } finally { $in.Dispose() }
    }
} finally { $zip.Dispose() }
Write-Host ("Packed {0} files into {1}" -f $want.Count, $Out)
