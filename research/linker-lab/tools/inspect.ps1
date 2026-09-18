[CmdletBinding()]
param()

$LabRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$BuildRoot = Join-Path $LabRoot 'build'
$InspectionRoot = Join-Path $BuildRoot 'inspection'

if (-not (Test-Path -LiteralPath (Join-Path $BuildRoot 'SUMMARY.txt'))) {
  throw 'No existe un build. Ejecuta primero tools\build.ps1.'
}

Get-Content -LiteralPath (Join-Path $BuildRoot 'SUMMARY.txt')

Write-Host "`n=== Miembros de liblabguard.a original ==="
Get-Content -LiteralPath (Join-Path $InspectionRoot 'original-ar-members.txt')

Write-Host "`n=== Binding original (GLOBAL) ==="
Select-String -LiteralPath (Join-Path $InspectionRoot 'original-object-readelf.txt') -Pattern 'lab_frame_sanity_check' |
  ForEach-Object { $_.Line }

Write-Host "`n=== Binding de la variante weak (WEAK) ==="
Select-String -LiteralPath (Join-Path $InspectionRoot 'weak-object-readelf.txt') -Pattern 'lab_frame_sanity_check' |
  ForEach-Object { $_.Line }

Write-Host "`n=== Símbolos que quedaron en cada ELF ==="
foreach ($variant in @('original', 'weak', 'wrap', 'replaced')) {
  Write-Host "[$variant]"
  Select-String -LiteralPath (Join-Path $InspectionRoot "$variant-final-nm.txt") -Pattern 'lab_frame_sanity_check|__wrap_lab_frame_sanity_check|labguard_archive_marker' |
    ForEach-Object { $_.Line }
}

Write-Host "`nLos disassemblies completos están en: $InspectionRoot"
