param(
    [string]$Configuration = "Release",
    [string]$SigningThumbprint = "",
    [string]$Version = "1.0.1",
    [switch]$UsePrebuiltEngine,
    [ValidateSet("Community", "Supporter")]
    [string]$Edition = "Community"
)

$ErrorActionPreference = "Stop"

function Assert-NativeSuccess {
    param([Parameter(Mandatory = $true)][string]$Step)

    if ($LASTEXITCODE -ne 0) {
        throw "$Step failed with exit code $LASTEXITCODE."
    }
}

$repo = Split-Path -Parent $PSScriptRoot
$engine = Join-Path $repo "client\engine"
$native = Join-Path $repo "client\native"
$editionSlug = $Edition.ToLowerInvariant()
$supporter = $Edition -eq "Supporter"
$build = Join-Path $repo "build\native-release-$editionSlug"
$engineBuild = Join-Path $repo "build\native-release-engine"
$enginePayload = Join-Path $engineBuild "OrbitLan.NetworkEngine.exe"
$dist = Join-Path $repo "dist"
$editionDir = Join-Path $dist $Edition
$packagePrefix = if ($supporter) { "OrbitLan-Supporter" } else { "OrbitLan" }
$installerName = "$packagePrefix-Installer.exe"
$portableName = "$packagePrefix-Portable.zip"
$installer = Join-Path $editionDir $installerName
$checksum = Join-Path $editionDir "$installerName.sha256"
$portable = Join-Path $editionDir "$packagePrefix-Portable"
$portableSupport = Join-Path $portable "support"
$portableZip = Join-Path $editionDir $portableName
$portableChecksum = Join-Path $editionDir "$portableName.sha256"

if (-not $UsePrebuiltEngine -and -not (Get-Command go -ErrorAction SilentlyContinue)) {
    throw "Go is required and was not found on PATH."
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "CMake with the Visual Studio C++ workload is required."
}

if (-not $UsePrebuiltEngine) {
    Push-Location $engine
    try {
        go fmt ./...
        Assert-NativeSuccess "go fmt"
        go test ./...
        Assert-NativeSuccess "go test"
        if (Test-Path $engineBuild) { Remove-Item $engineBuild -Recurse -Force }
        New-Item -ItemType Directory -Path $engineBuild -Force | Out-Null
        $env:GOOS = "windows"
        $env:GOARCH = "amd64"
        go build -trimpath -ldflags "-s -w" -o $enginePayload .
        Assert-NativeSuccess "go build"
    }
    finally {
        Pop-Location
    }
} elseif (-not (Test-Path $enginePayload)) {
    throw "Prebuilt engine not found at $enginePayload."
}

$engineHeader = [IO.File]::ReadAllBytes($enginePayload)
if ($engineHeader.Length -lt 2 -or $engineHeader[0] -ne 0x4d -or $engineHeader[1] -ne 0x5a) {
    throw "OrbitLan.NetworkEngine.exe is not a Windows PE executable."
}

if (Test-Path $build) { Remove-Item $build -Recurse -Force }
$editionSwitch = if ($supporter) { "ON" } else { "OFF" }
cmake -S $native -B $build -A x64 "-DORBITLAN_ENGINE_PAYLOAD=$enginePayload" `
    "-DORBITLAN_SUPPORTER_EDITION=$editionSwitch"
Assert-NativeSuccess "CMake configure"
cmake --build $build --config $Configuration `
    --target OrbitLan OrbitLanService OrbitLanPortableSetup OrbitLanNativeTests --parallel 1
Assert-NativeSuccess "CMake native build"
ctest --test-dir $build -C $Configuration --output-on-failure
Assert-NativeSuccess "Native tests"

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
    Assert-NativeSuccess "Signing native binaries"
}

cmake --build $build --config $Configuration --target OrbitLanInstaller --parallel 1
Assert-NativeSuccess "Installer build"

if ($SigningThumbprint) {
    & $signTool.Source sign /sha1 $SigningThumbprint /fd SHA256 /td SHA256 `
        /tr "http://timestamp.digicert.com" (Join-Path $bin "OrbitLan-Installer.exe")
    Assert-NativeSuccess "Signing installer"
} else {
    Write-Warning "Native binaries are unsigned. Pass -SigningThumbprint for a public production release."
}

if (Test-Path $editionDir) { Remove-Item $editionDir -Recurse -Force }
New-Item -ItemType Directory -Path $editionDir -Force | Out-Null
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

if ($supporter) {
    $bundleName = "OrbitLan-Supporter-v$Version.zip"
    $bundle = Join-Path $editionDir $bundleName
    $bundleChecksum = Join-Path $editionDir "$bundleName.sha256"
    $bundleStage = Join-Path $editionDir "supporter-bundle"
    $bundleInstaller = Join-Path $bundleStage "Installer"
    $bundlePortable = Join-Path $bundleStage "Portable"
    $bundleLinux = Join-Path $bundleStage "Linux"
    $linuxPackages = Join-Path $dist "linux\supporter"

    New-Item -ItemType Directory -Path $bundleInstaller -Force | Out-Null
    New-Item -ItemType Directory -Path $bundlePortable -Force | Out-Null
    New-Item -ItemType Directory -Path $bundleLinux -Force | Out-Null
    Copy-Item (Join-Path $repo "docs\SUPPORTER-DOWNLOAD.txt") `
        (Join-Path $bundleStage "START-HERE.txt")
    Copy-Item $installer,$checksum $bundleInstaller
    Copy-Item (Join-Path $portable "*") $bundlePortable -Recurse -Force
    if (-not (Test-Path $linuxPackages)) {
        throw "Linux Supporter packages are missing. Run EDITION=supporter VERSION=$Version ./scripts/build-linux-release.sh first."
    }
    Copy-Item (Join-Path $linuxPackages "*.tar.gz") $bundleLinux -Force
    Copy-Item (Join-Path $linuxPackages "*.sha256") $bundleLinux -Force

    Compress-Archive -Path (Join-Path $bundleStage "*") -DestinationPath $bundle `
        -CompressionLevel Optimal
    $bundleHash = (Get-FileHash $bundle -Algorithm SHA256).Hash.ToLowerInvariant()
    "$bundleHash  $bundleName" | Set-Content $bundleChecksum -Encoding ascii
    Remove-Item $bundleStage -Recurse -Force
    Write-Host "Patreon bundle: $bundle"
}

Write-Host "Built $Edition edition: $installer"
Write-Host "Built $Edition edition: $portableZip"
