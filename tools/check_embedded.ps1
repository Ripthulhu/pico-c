param(
    [string]$Compiler = 'C:/Espressif/tools/xtensa-esp-elf/esp-14.2.0_20241119/xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc.exe',
    [switch]$UpdateEvidence
)

$ErrorActionPreference = 'Stop'
$projectDirectory = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$buildDirectory = Join-Path $projectDirectory 'build/crosscheck/esp32s3'
$evidenceDirectory = Join-Path $projectDirectory 'docs/evidence/esp32s3'
$toolDirectory = Split-Path -Parent (Resolve-Path -LiteralPath $Compiler).Path
$Compiler = Join-Path $toolDirectory 'xtensa-esp32s3-elf-gcc.exe'
$sizeTool = Join-Path $toolDirectory 'xtensa-esp32s3-elf-size.exe'
$nmTool = Join-Path $toolDirectory 'xtensa-esp32s3-elf-nm.exe'
$readelfTool = Join-Path $toolDirectory 'xtensa-esp32s3-elf-readelf.exe'
$null = New-Item -ItemType Directory -Force -Path $buildDirectory

function Save-Text([string]$Name, [object]$Value) {
    $lines = (($Value -join "`n") -split "`r?`n") | ForEach-Object { $_.TrimEnd() }
    $text = ($lines -join "`n").TrimEnd() + "`n"
    [IO.File]::WriteAllText((Join-Path $buildDirectory $Name), $text, [Text.UTF8Encoding]::new($false))
}

function Invoke-Checked([string]$Tool, [string[]]$Arguments) {
    $result = & $Tool @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Tool failed with exit code $LASTEXITCODE"
    }
    return $result
}

$flags = @(
    '-std=c99', '-Os', '-ffreestanding', '-fno-builtin',
    '-ffunction-sections', '-fdata-sections', '-fstack-usage',
    '-Wall', '-Wextra', '-Werror', '-pedantic',
    '-DPICO_MAX_INSTANCES=320', '-DPICO_MAX_QUEUE=128',
    '-DPICO_RENDER_MAX_WIDTH=128', '-Iinclude', '-Igenerated'
)
$coreSources = @('src/pico.c', 'src/pico_render.c', 'src/pico_ui.c', 'generated/game_data.c')
$sources = $coreSources + 'src/pico_audio.c'
$headers = @('include/pico.h', 'include/pico_render.h', 'include/pico_ui.h',
             'include/pico_audio.h', 'generated/game_data.h')
$outputDirectory = 'build/crosscheck/esp32s3'

Push-Location $projectDirectory
try {
    Save-Text 'toolchain-version.txt' (Invoke-Checked $Compiler @('--version'))
    Save-Text 'flags.txt' $flags
    $objects = @()
    foreach ($source in $sources) {
        $object = "$outputDirectory/$([IO.Path]::GetFileNameWithoutExtension($source)).o"
        Invoke-Checked $Compiler ($flags + @('-c', $source, '-o', $object))
        $objects += $object
    }
    $combined = "$outputDirectory/combined.o"
    $withAudio = "$outputDirectory/combined-audio.o"
    Invoke-Checked $Compiler (@('-r', '-nostdlib') + $objects[0..3] + @('-o', $combined))
    Invoke-Checked $Compiler (@('-r', '-nostdlib') + $objects + @('-o', $withAudio))
    Save-Text 'size-summary.txt' (Invoke-Checked $sizeTool ($objects + @($combined, $withAudio)))
    Save-Text 'sections.txt' (Invoke-Checked $sizeTool (@('-A') + $objects + @($combined, $withAudio)))
    Save-Text 'undefined.txt' (Invoke-Checked $nmTool @('-u', $combined))
    Save-Text 'undefined-audio.txt' (Invoke-Checked $nmTool @('-u', $withAudio))
    Save-Text 'elf-abi.txt' (Invoke-Checked $readelfTool @('-h', '-A', $combined))
    Save-Text 'target-options.txt' (Invoke-Checked $Compiler @('-Q', '--help=target'))
    Save-Text 'empty.c' ''
    Save-Text 'predefined-macros.txt' (Invoke-Checked $Compiler @('-dM', '-E', "$outputDirectory/empty.c"))

    Save-Text 'layout_probe.c' @'
#include "pico.h"
#include "pico_render.h"
#include "pico_audio.h"
unsigned char pico_context_bytes[sizeof(Pico)];
unsigned char pico_instance_bytes[sizeof(PicoInstance)];
unsigned char pico_queue_bytes[sizeof(PicoQueued)];
unsigned char pico_draw_bytes[sizeof(PicoDraw)];
unsigned char pico_assets_bytes[sizeof(PicoAssets)];
unsigned char pico_data_bytes[sizeof(PicoData)];
unsigned char pico_frame_bytes[sizeof(PicoFrame)];
unsigned char pico_pointer_bytes[sizeof(void*)];
unsigned char pico_int64_bytes[sizeof(int64_t)];
unsigned char pico_sound_event_bytes[sizeof(PicoSoundEvent)];
unsigned char pico_sound_envelope_bytes[sizeof(PicoSoundEnvelope)];
unsigned char pico_audio_bytes[sizeof(PicoAudio)];
unsigned char pico_audio_voice_bytes[sizeof(PicoAudioVoice)];
'@
    Invoke-Checked $Compiler ($flags + @('-c', "$outputDirectory/layout_probe.c", '-o', "$outputDirectory/layout_probe.o"))
    Save-Text 'layout.txt' (Invoke-Checked $nmTool @('-S', '--size-sort', '--radix=d', "$outputDirectory/layout_probe.o"))
    Invoke-Checked $Compiler ($flags + @('-DPICO_AUDIO_MAX_VOICES=4', '-c', "$outputDirectory/layout_probe.c", '-o', "$outputDirectory/layout_4voices.o"))
    Save-Text 'layout-4voices.txt' (Invoke-Checked $nmTool @('-S', '--size-sort', '--radix=d', "$outputDirectory/layout_4voices.o"))

    $hashRecords = foreach ($source in ($sources + $headers)) {
        $absoluteSource = Join-Path $projectDirectory $source
        $sourceBytes = [IO.File]::ReadAllBytes($absoluteSource)
        $lfBytes = [Text.Encoding]::UTF8.GetBytes(([Text.Encoding]::UTF8.GetString($sourceBytes) -replace "`r`n", "`n"))
        $hasher = [Security.Cryptography.SHA256]::Create()
        try {
            [ordered]@{
                path = $source
                sha256 = ([BitConverter]::ToString($hasher.ComputeHash($sourceBytes))).Replace('-', '').ToLowerInvariant()
                sha256_lf = ([BitConverter]::ToString($hasher.ComputeHash($lfBytes))).Replace('-', '').ToLowerInvariant()
            }
        } finally {
            $hasher.Dispose()
        }
    }
    Save-Text 'source-hashes.json' ($hashRecords | ConvertTo-Json -Depth 3)
    if ($UpdateEvidence) {
        $null = New-Item -ItemType Directory -Force -Path $evidenceDirectory
        $reports = @('size-summary.txt', 'sections.txt', 'undefined.txt', 'undefined-audio.txt',
                     'layout.txt', 'layout-4voices.txt', 'elf-abi.txt', 'target-options.txt',
                     'predefined-macros.txt', 'source-hashes.json', 'toolchain-version.txt', 'flags.txt')
        $reports += $sources | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_) + '.su' }
        foreach ($report in $reports) {
            Copy-Item -LiteralPath (Join-Path $buildDirectory $report) -Destination (Join-Path $evidenceDirectory $report)
        }
    }
    Get-Content -LiteralPath (Join-Path $buildDirectory 'size-summary.txt')
    Get-Content -LiteralPath (Join-Path $buildDirectory 'layout.txt')
} finally {
    Pop-Location
}
