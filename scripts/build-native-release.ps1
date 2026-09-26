param(
    [string]$Configuration = "Release",
    [string]$SigningThumbprint = "",
    [ValidateSet("Community", "Supporter")]
    [string]$Edition = "Community"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$engine = Join-Path $repo "client\engine"
$native = Join-Path $repo "client\native"
$editionSlug = $Edition.ToLowerInvariant()
$supporter = $Edition -eq "Supporter"
$build = Join-Path $repo "build\native-release-$editionSlug"
$engineBuild = Join-Path $repo "build\native-release-engine"
$enginePayload = Join-Path $engineBuild "OrbitLan.NetworkEngine.exe"
$dist = Join-Path $repo "dist"
$packagePrefix = if ($supporter) { "OrbitLan-Supporter" } else { "OrbitLan" }
$installerName = "$packagePrefix-Installer.exe"
$portableName = "$packagePrefix-Portable.zip"
$installer = Join-Path $dist $installerName
$checksum = Join-Path $dist "$installerName.sha256"
$portable = Join-Path $dist "$packagePrefix-Portable"
$portableSupport = Join-Path $portable "support"
$portableZip = Join-Path $dist $portableName
$portableChecksum = Join-Path $dist "$portableName.sha256"

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
    if (Test-Path $engineBuild) { Remove-Item $engineBuild -Recurse -Force }
    New-Item -ItemType Directory -Path $engineBuild -Force | Out-Null
    $env:GOOS = "windows"
    $env:GOARCH = "amd64"
    go build -trimpath -ldflags "-s -w" -o $enginePayload .
}
finally {
    Pop-Location
}

$engineHeader = [IO.File]::ReadAllBytes($enginePayload)
if ($engineHeader.Length -lt 2 -or $engineHeader[0] -ne 0x4d -or $engineHeader[1] -ne 0x5a) {
    throw "OrbitLan.NetworkEngine.exe is not a Windows PE executable."
}

if (Test-Path $build) { Remove-Item $build -Recurse -Force }
$editionSwitch = if ($supporter) { "ON" } else { "OFF" }
cmake -S $native -B $build -A x64 "-DORBITLAN_ENGINE_PAYLOAD=$enginePayload" `
    "-DORBITLAN_SUPPORTER_EDITION=$editionSwitch"
cmake --build $build --config $Configuration `
    --target OrbitLan OrbitLanService OrbitLanPortableSetup OrbitLanNativeTests --parallel
ctest --test-dir $build -C $Configuration --output-on-failure

$bin = Join-Path $build "bin"

if ($SigningThumbprint) {
    $signTool = Get-Command signtool.exe -ErrorAction Stop
    $signTargets = @(
        (Join-Path $bin "OrbitLan.exe"),
        (Join-Path $bin "OrbitLan.NetworkService.exe"),
        (Join-Path $bin "OrbitLan.Setup.exe"),
        $enginePayload
    )
    & $signTool.Source sign /sha1 $SigningThumbprint /fd SHA256 /td SHA256 `
        /tr "http://timestamp.digicert.com" $signTargets
}

cmake --build $build --config $Configuration --target OrbitLanInstaller --parallel

if ($SigningThumbprint) {
    & $signTool.Source sign /sha1 $SigningThumbprint /fd SHA256 /td SHA256 `
        /tr "http://timestamp.digicert.com" (Join-Path $bin "OrbitLan-Installer.exe")
} else {
    Write-Warning "Native binaries are unsigned. Pass -SigningThumbprint for a public production release."
}

New-Item -ItemType Directory -Path $dist -Force | Out-Null
Copy-Item (Join-Path $bin "OrbitLan-Installer.exe") $installer -Force
$hash = (Get-FileHash $installer -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $installerName" | Set-Content $checksum -Encoding ascii

if (Test-Path $portable) { Remove-Item $portable -Recurse -Force }
if (Test-Path $portableZip) { Remove-Item $portableZip -Force }
New-Item -ItemType Directory -Path (Join-Path $portableSupport "assets") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $portableSupport "driver") -Force | Out-Null
Copy-Item (Join-Path $bin "OrbitLan.exe") $portable
Copy-Item (Join-Path $bin "OrbitLan.NetworkService.exe") $portableSupport
Copy-Item (Join-Path $bin "OrbitLan.Setup.exe") $portableSupport
Copy-Item $enginePayload (Join-Path $portableSupport "OrbitLan.NetworkEngine.exe")
Copy-Item (Join-Path $repo "client\ui\assets\earth-clouds.gif") (Join-Path $portableSupport "assets")
Copy-Item (Join-Path $repo "client\ui\assets\earth-header-sheet.png") (Join-Path $portableSupport "assets")
Copy-Item (Join-Path $repo "client\ui\assets\moon-supporter.png") (Join-Path $portableSupport "assets")
Copy-Item (Join-Path $repo "client\ui\payload\driver\*") (Join-Path $portableSupport "driver") -Force
Compress-Archive -Path $portable -DestinationPath $portableZip -CompressionLevel Optimal
$portableHash = (Get-FileHash $portableZip -Algorithm SHA256).Hash.ToLowerInvariant()
"$portableHash  $portableName" | Set-Content $portableChecksum -Encoding ascii

Write-Host "Built $Edition edition: $installer"
Write-Host "Built $Edition edition: $portableZip"
