param([ValidateSet('reference','lowmemory')][string]$Profile = 'reference',
      [string]$Distribution = 'Ubuntu-24.04',
      [ValidateRange(0,3840)][int]$Width = 0,
      [ValidateRange(0,4096)][int]$Height = 0,
      [ValidateRange(0,16)][int]$Scale = 0,
      [string]$Assets = '',
      [string]$Sounds = '',
      [switch]$Mute,
      [switch]$Mono)
$ErrorActionPreference = 'Stop'
if ($Profile -eq 'lowmemory') {
    if (!$Width) { $Width = 96 }
    if (!$Height) { $Height = 64 }
    if (!$Scale) { $Scale = 8 }
    if ($Width -gt 128) { throw 'The lowmemory profile supports widths up to 128. Use -Profile reference for larger screens.' }
    $assetPack = 'assets/pico_art_micro.pcta'
} else {
    if (!$Width) { $Width = 550 }
    if (!$Height) { $Height = [Math]::Max(1, [Math]::Floor($Width * 350 / 550)) }
    if (!$Scale) { $Scale = if ($Width -le 550 -and $Height -le 350) { 2 } else { 1 } }
    $assetPack = 'assets/pico_art_rgba.pcta'
}
if ($Assets) { $assetPack = $Assets }
$projectPath = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$linuxPath = (& wsl.exe -d $Distribution -- wslpath -a $projectPath).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve the project directory through WSL.' }
$hostArguments = @('-d', $Distribution, '--cd', $linuxPath, '--', "build/$Profile/pico-x11",
    '--assets', $assetPack, '--width', "$Width", '--height', "$Height", '--scale', "$Scale")
if ($Mono) { $hostArguments += '--mono' }
if ($Mute) { $hostArguments += '--mute' }
if ($Sounds) { $hostArguments += @('--sounds', $Sounds) }
& wsl.exe @hostArguments
if ($LASTEXITCODE -ne 0) { throw "Game exited with an error (exit $LASTEXITCODE). Run tools/build.ps1 first if it is not built." }
