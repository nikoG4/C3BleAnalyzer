[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$LabRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$BuildRoot = [IO.Path]::GetFullPath((Join-Path $LabRoot 'build'))
$ArduinoCli = 'C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
$ToolchainBin = Join-Path $env:LOCALAPPDATA 'Arduino15\packages\esp32\tools\esp-rv32\2411\bin'
$Cc = Join-Path $ToolchainBin 'riscv32-esp-elf-gcc.exe'
$Ar = Join-Path $ToolchainBin 'riscv32-esp-elf-ar.exe'
$Ranlib = Join-Path $ToolchainBin 'riscv32-esp-elf-ranlib.exe'
$Nm = Join-Path $ToolchainBin 'riscv32-esp-elf-nm.exe'
$ReadElf = Join-Path $ToolchainBin 'riscv32-esp-elf-readelf.exe'
$ObjDump = Join-Path $ToolchainBin 'riscv32-esp-elf-objdump.exe'

foreach ($tool in @($ArduinoCli, $Cc, $Ar, $Ranlib, $Nm, $ReadElf, $ObjDump)) {
  if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
    throw "No se encontró la herramienta requerida: $tool"
  }
}

if (-not $BuildRoot.StartsWith($LabRoot, [StringComparison]::OrdinalIgnoreCase)) {
  throw "Ruta de build fuera del laboratorio: $BuildRoot"
}

if (Test-Path -LiteralPath $BuildRoot) {
  Remove-Item -LiteralPath $BuildRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null

$IncludeDir = Join-Path $LabRoot 'original\include'
$OriginalSource = Join-Path $LabRoot 'original\src\labguard.c'
$MarkerSource = Join-Path $LabRoot 'original\src\archive_marker.c'
$WeakSource = Join-Path $LabRoot 'patched\weak\labguard_weak.c'
$PatchedSource = Join-Path $LabRoot 'patched\src\labguard.c'

$CFlags = @(
  '-march=rv32imc_zicsr_zifencei',
  '-mabi=ilp32',
  '-Os',
  '-g3',
  '-ffunction-sections',
  '-fdata-sections',
  '-fno-jump-tables',
  '-fno-tree-switch-conversion',
  '-std=gnu17',
  "-I$IncludeDir"
)

function Invoke-Checked {
  param(
    [Parameter(Mandatory)][string]$FilePath,
    [Parameter(Mandatory)][string[]]$Arguments
  )

  & $FilePath @Arguments
  if ($LASTEXITCODE -ne 0) {
    throw "Falló ($LASTEXITCODE): $FilePath $($Arguments -join ' ')"
  }
}

function Invoke-Captured {
  param(
    [Parameter(Mandatory)][string]$FilePath,
    [Parameter(Mandatory)][string[]]$Arguments,
    [Parameter(Mandatory)][string]$OutputPath
  )

  $parent = Split-Path -Parent $OutputPath
  New-Item -ItemType Directory -Force -Path $parent | Out-Null
  & $FilePath @Arguments 2>&1 | Out-File -LiteralPath $OutputPath -Encoding utf8
  if ($LASTEXITCODE -ne 0) {
    throw "Falló ($LASTEXITCODE): $FilePath $($Arguments -join ' ')"
  }
}

function Compile-CObject {
  param([string]$Source, [string]$Output)
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Output) | Out-Null
  Invoke-Checked $Cc ($CFlags + @('-c', $Source, '-o', $Output))
}

$ObjectsRoot = Join-Path $BuildRoot 'objects'
$ArchivesRoot = Join-Path $BuildRoot 'archives'
$InspectionRoot = Join-Path $BuildRoot 'inspection'

$OriginalObject = Join-Path $ObjectsRoot 'original\labguard.o'
$MarkerObject = Join-Path $ObjectsRoot 'common\archive_marker.o'
$WeakObject = Join-Path $ObjectsRoot 'weak\labguard.o'
$PatchedObject = Join-Path $ObjectsRoot 'patched\labguard.o'

Compile-CObject $OriginalSource $OriginalObject
Compile-CObject $MarkerSource $MarkerObject
Compile-CObject $WeakSource $WeakObject
Compile-CObject $PatchedSource $PatchedObject

foreach ($variant in @('original', 'weak', 'replaced')) {
  New-Item -ItemType Directory -Force -Path (Join-Path $ArchivesRoot $variant) | Out-Null
}

$OriginalArchive = Join-Path $ArchivesRoot 'original\liblabguard.a'
$WeakArchive = Join-Path $ArchivesRoot 'weak\liblabguard.a'
$ReplacedArchive = Join-Path $ArchivesRoot 'replaced\liblabguard.a'

Invoke-Checked $Ar @('rcs', $OriginalArchive, $OriginalObject, $MarkerObject)
Invoke-Checked $Ranlib @($OriginalArchive)
Invoke-Checked $Ar @('rcs', $WeakArchive, $WeakObject, $MarkerObject)
Invoke-Checked $Ranlib @($WeakArchive)

$ExtractedDir = Join-Path $BuildRoot 'extracted\original'
New-Item -ItemType Directory -Force -Path $ExtractedDir | Out-Null
Push-Location $ExtractedDir
try {
  Invoke-Checked $Ar @('x', $OriginalArchive)
}
finally {
  Pop-Location
}

Copy-Item -LiteralPath $OriginalArchive -Destination $ReplacedArchive -Force
Invoke-Checked $Ar @('d', $ReplacedArchive, 'labguard.o')
Invoke-Checked $Ar @('r', $ReplacedArchive, $PatchedObject)
Invoke-Checked $Ranlib @($ReplacedArchive)

Invoke-Captured $Ar @('t', $OriginalArchive) (Join-Path $InspectionRoot 'original-ar-members.txt')
Invoke-Captured $Nm @('-A', '-C', '--defined-only', $OriginalArchive) (Join-Path $InspectionRoot 'original-archive-nm.txt')
Invoke-Captured $ReadElf @('-Ws', $OriginalObject) (Join-Path $InspectionRoot 'original-object-readelf.txt')
Invoke-Captured $ObjDump @('-d', '-C', $OriginalObject) (Join-Path $InspectionRoot 'original-object-objdump.txt')
Invoke-Captured $Nm @('-A', '-C', '--defined-only', $WeakArchive) (Join-Path $InspectionRoot 'weak-archive-nm.txt')
Invoke-Captured $ReadElf @('-Ws', $WeakObject) (Join-Path $InspectionRoot 'weak-object-readelf.txt')
Invoke-Captured $Ar @('t', $ReplacedArchive) (Join-Path $InspectionRoot 'replaced-ar-members.txt')
Invoke-Captured $Nm @('-A', '-C', '--defined-only', $ReplacedArchive) (Join-Path $InspectionRoot 'replaced-archive-nm.txt')
Invoke-Captured $ObjDump @('-d', '-C', $PatchedObject) (Join-Path $InspectionRoot 'patched-object-objdump.txt')

function New-LocalArduinoLibrary {
  param(
    [string]$Variant,
    [string]$Archive,
    [switch]$Wrap
  )

  $libraryRoot = Join-Path $BuildRoot "libraries\$Variant"
  $libraryDir = Join-Path $libraryRoot 'LabGuard'
  $sourceDir = Join-Path $libraryDir 'src'
  $archDir = Join-Path $sourceDir 'esp32c3'
  New-Item -ItemType Directory -Force -Path $archDir | Out-Null
  Copy-Item -LiteralPath (Join-Path $IncludeDir 'labguard.h') -Destination (Join-Path $sourceDir 'labguard.h') -Force
  Copy-Item -LiteralPath $Archive -Destination (Join-Path $archDir 'liblabguard.a') -Force
  $properties = if ($Wrap) { 'library-wrap.properties' } else { 'library.properties' }
  Copy-Item -LiteralPath (Join-Path $PSScriptRoot $properties) -Destination (Join-Path $libraryDir 'library.properties') -Force
  return $libraryRoot
}

$LibraryRoots = @{}
$LibraryRoots.original = New-LocalArduinoLibrary 'original' $OriginalArchive
$LibraryRoots.weak = New-LocalArduinoLibrary 'weak' $WeakArchive
$LibraryRoots.wrap = New-LocalArduinoLibrary 'wrap' $OriginalArchive -Wrap
$LibraryRoots.replaced = New-LocalArduinoLibrary 'replaced' $ReplacedArchive

$Sketches = @{
  original = Join-Path $LabRoot 'original\LinkerLabOriginal'
  weak = Join-Path $LabRoot 'patched\weak\LinkerLabWeak'
  wrap = Join-Path $LabRoot 'patched\wrap\LinkerLabWrap'
  replaced = Join-Path $LabRoot 'patched\replaced\LinkerLabReplaced'
}

function Build-Firmware {
  param([string]$Variant)

  $buildPath = Join-Path $BuildRoot "work\$Variant"
  $outputPath = Join-Path $BuildRoot "firmware\$Variant"
  $logPath = Join-Path $BuildRoot "logs\$Variant-build.txt"
  New-Item -ItemType Directory -Force -Path $buildPath, $outputPath, (Split-Path -Parent $logPath) | Out-Null

  $arguments = @(
    'compile',
    '--fqbn', 'esp32:esp32:esp32c3',
    '--build-path', $buildPath,
    '--output-dir', $outputPath,
    '--libraries', $LibraryRoots[$Variant],
    '--warnings', 'all',
    $Sketches[$Variant]
  )
  Invoke-Captured $ArduinoCli $arguments $logPath

  $elf = Get-ChildItem -LiteralPath $outputPath -Filter '*.elf' | Select-Object -First 1
  $map = Get-ChildItem -LiteralPath $outputPath -Filter '*.map' | Select-Object -First 1
  if (-not $elf -or -not $map) {
    throw "El build $Variant no produjo ELF/MAP"
  }

  Invoke-Captured $Nm @('-C', '--defined-only', $elf.FullName) (Join-Path $InspectionRoot "$Variant-final-nm.txt")
  Invoke-Captured $ReadElf @('-Ws', $elf.FullName) (Join-Path $InspectionRoot "$Variant-final-readelf.txt")
  Select-String -LiteralPath $map.FullName -Pattern 'lab_frame_sanity_check|__wrap_lab_frame_sanity_check|liblabguard.a|archive_marker' |
    ForEach-Object { $_.Line } |
    Out-File -LiteralPath (Join-Path $InspectionRoot "$Variant-map-symbols.txt") -Encoding utf8
}

foreach ($variant in @('original', 'weak', 'wrap', 'replaced')) {
  Build-Firmware $variant
}

$summary = Join-Path $BuildRoot 'SUMMARY.txt'
@(
  'ESP32-C3 static-linking laboratory build completed.',
  "Generated: $(Get-Date -Format o)",
  '',
  'Final symbols:'
) | Out-File -LiteralPath $summary -Encoding utf8

foreach ($variant in @('original', 'weak', 'wrap', 'replaced')) {
  Add-Content -LiteralPath $summary -Value "`n[$variant]"
  Select-String -LiteralPath (Join-Path $InspectionRoot "$variant-final-nm.txt") -Pattern 'lab_frame_sanity_check|archive_marker' |
    ForEach-Object { $_.Line } |
    Add-Content -LiteralPath $summary
  Get-ChildItem -LiteralPath (Join-Path $BuildRoot "firmware\$variant") -Filter '*.bin' |
    ForEach-Object {
      $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName
      Add-Content -LiteralPath $summary -Value ("SHA256 {0}  {1}" -f $hash.Hash, $_.FullName)
    }
}

Write-Host "Laboratorio compilado: $BuildRoot"
Write-Host "Resumen: $summary"
