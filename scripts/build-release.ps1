param(
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$engine = Join-Path $repo "client\engine"
$ui = Join-Path $repo "client\ui"
$engineExe = Join-Path $ui "payload\orbitlan-engine.exe"
$publish = Join-Path $ui "publish"
$dist = Join-Path $repo "dist"
$stage = Join-Path $dist "OrbitLan"
$zip = Join-Path $dist "OrbitLan.zip"

if (-not (Get-Command go -ErrorAction SilentlyContinue)) {
    throw "Go is required and was not found on PATH."
}
if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    throw ".NET 10 SDK is required and was not found on PATH."
}

Push-Location $engine
try {
    go fmt ./...
    go test ./...
    go build -trimpath -ldflags "-s -w" -o $engineExe .
}
finally {
    Pop-Location
}

if (Test-Path $publish) { Remove-Item $publish -Recurse -Force }
dotnet publish (Join-Path $ui "OrbitLan.csproj") `
    -c $Configuration `
    -r win-x64 `
    --self-contained true `
    -p:PublishSingleFile=true `
    -p:DebugType=None `
    -o $publish

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
if (Test-Path $zip) { Remove-Item $zip -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Copy-Item (Join-Path $publish "*") $stage -Recurse -Force
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal

Write-Host "Built $zip"
