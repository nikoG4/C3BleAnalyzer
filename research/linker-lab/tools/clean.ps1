[CmdletBinding()]
param()

$LabRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$BuildRoot = [IO.Path]::GetFullPath((Join-Path $LabRoot 'build'))

if (-not $BuildRoot.StartsWith($LabRoot, [StringComparison]::OrdinalIgnoreCase)) {
  throw "Ruta fuera del laboratorio: $BuildRoot"
}

if (Test-Path -LiteralPath $BuildRoot) {
  Remove-Item -LiteralPath $BuildRoot -Recurse -Force
  Write-Host "Eliminado: $BuildRoot"
} else {
  Write-Host "No existe: $BuildRoot"
}
