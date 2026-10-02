#requires -Version 5.1
param(
    [string]$OutputDir = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path $PSScriptRoot -Parent
$packagesRoot = Join-Path $env:LOCALAPPDATA "Arduino15\packages\esp32"
$coreDir = Join-Path $packagesRoot "hardware\esp32\3.3.0"
$archive = Join-Path $packagesRoot "tools\esp32-arduino-libs\idf-release_v5.5-b66b5448-v1\esp32c3\lib\libnet80211.a"
$expectedHash = "2929D7B8634D7570D4E2466B1B518CF2D63652E6EB1D7A6862F5415040227A8A"

if (-not (Get-Command arduino-cli -ErrorAction SilentlyContinue)) {
    throw "Instala Arduino CLI y agregalo al PATH."
}
if (-not (Test-Path -LiteralPath $coreDir)) {
    throw "Esta compilacion requiere Arduino-ESP32 3.3.0 (esp32:esp32)."
}
if (-not (Test-Path -LiteralPath $archive)) {
    throw "No se encontro libnet80211.a de ESP32-C3 / IDF 5.5 esperado."
}
$actualHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
if ($actualHash -ne $expectedHash) {
    throw "La biblioteca Wi-Fi difiere de la version analizada. SHA-256: $actualHash"
}

if (-not $OutputDir) {
    $OutputDir = Join-Path $repoRoot ".build\lab-firmware"
}
$sketchDir = Join-Path $repoRoot ".build\lab-sketch\C3BleAnalyzer"
New-Item -ItemType Directory -Path $sketchDir, $OutputDir -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "C3BleAnalyzer.ino") -Destination (Join-Path $sketchDir "C3BleAnalyzer.ino") -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "lab_raw_tx.cpp") -Destination (Join-Path $sketchDir "lab_raw_tx.cpp") -Force

# El simbolo sustituto solo se compila aqui. Arduino IDE conserva el build normal.
# COM por USB nativo del C3: Serial debe usar USB CDC, no UART0.
& arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc --warnings all `
    --output-dir $OutputDir `
    --build-property "compiler.cpp.extra_flags=-DLAB_ENABLE_RAW_TX" `
    --build-property "compiler.c.elf.extra_flags=-Wl,--allow-multiple-definition" `
    $sketchDir
if ($LASTEXITCODE -ne 0) {
    throw "Fallo la compilacion LAB ($LASTEXITCODE)."
}

$binary = Join-Path $OutputDir "C3BleAnalyzer.ino.bin"
if (-not (Test-Path -LiteralPath $binary)) {
    throw "Arduino CLI termino sin crear el binario esperado: $binary"
}
Write-Host "Firmware LAB creado: $binary"
Write-Host "Sin prueba fisica: ESP_OK no demuestra que la trama haya salido al aire."
