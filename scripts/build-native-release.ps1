param(
    [string]$Configuration = "Release",
    [string]$SigningThumbprint = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$engine = Join-Path $repo "client\engine"
$native = Join-Path $repo "client\native"
$build = Join-Path $repo "build\native-release"
$dist = Join-Path $repo "dist"
$stage = Join-Path $dist "OrbitLan-Native"
$zip = Join-Path $dist "OrbitLan-Native.zip"

if (-not (Get-Command go -ErrorAction SilentlyContinue)) {
    throw "Go is required and was not found on PATH."
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "CMake with the Visual Studio C++ workload is required."
}

Push-Location $engine
try {
    go fmt ./...
    go test ./...
}
finally {
    Pop-Location
}

if (Test-Path $build) { Remove-Item $build -Recurse -Force }
cmake -S $native -B $build -A x64
cmake --build $build --config $Configuration --parallel

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
if (Test-Path $zip) { Remove-Item $zip -Force }
New-Item -ItemType Directory -Path (Join-Path $stage "driver") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $stage "assets") -Force | Out-Null

$bin = Join-Path $build "bin"
Copy-Item (Join-Path $bin "OrbitLan.exe") $stage
Copy-Item (Join-Path $bin "OrbitLanService.exe") $stage
Copy-Item (Join-Path $bin "OrbitLanSetup.exe") $stage

Push-Location $engine
try {
    go build -trimpath -ldflags "-s -w" -o (Join-Path $stage "orbitlan-engine.exe") .
}
finally {
    Pop-Location
}

Copy-Item (Join-Path $repo "client\ui\payload\driver\*") (Join-Path $stage "driver") -Force
Copy-Item (Join-Path $repo "client\ui\assets\earth-clouds.gif") (Join-Path $stage "assets") -Force

if ($SigningThumbprint) {
    $signTool = Get-Command signtool.exe -ErrorAction Stop
    $signTargets = @(
        (Join-Path $stage "OrbitLan.exe"),
        (Join-Path $stage "OrbitLanService.exe"),
        (Join-Path $stage "OrbitLanSetup.exe"),
        (Join-Path $stage "orbitlan-engine.exe")
    )
    & $signTool.Source sign /sha1 $SigningThumbprint /fd SHA256 /td SHA256 `
        /tr "http://timestamp.digicert.com" $signTargets
} else {
    Write-Warning "Native binaries are unsigned. Pass -SigningThumbprint for a public production release."
}

$hashes = Get-ChildItem $stage -File -Recurse | Sort-Object FullName | ForEach-Object {
    $relative = [IO.Path]::GetRelativePath($stage, $_.FullName).Replace('\', '/')
    $hash = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $relative"
}
$hashes | Set-Content (Join-Path $stage "SHA256SUMS.txt") -Encoding ascii
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal

Write-Host "Built $zip"
