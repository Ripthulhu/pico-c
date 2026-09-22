param([ValidateSet('reference','lowmemory')][string]$Profile = 'reference',
      [string]$Distribution = 'Ubuntu-24.04')
$ErrorActionPreference = 'Stop'
$projectPath = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$linuxPath = (& wsl.exe -d $Distribution -- wslpath -a $projectPath).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve the project directory through WSL.' }
& wsl.exe -d $Distribution --cd $linuxPath -- sh tools/build.sh $Profile
if ($LASTEXITCODE -ne 0) { throw "Build or checks failed (exit $LASTEXITCODE)." }
