$ErrorActionPreference = "Stop"

$repo = "Atiqxyz/vex"
$asset = "vex-windows-x64.zip"
$baseUrl = if ($env:VEX_RELEASE_BASE_URL) { $env:VEX_RELEASE_BASE_URL.TrimEnd("/") } else { "https://github.com/$repo/releases/latest/download" }
$tempRoot = Join-Path $env:TEMP ("vex-install-" + [guid]::NewGuid().ToString("N"))
$archive = Join-Path $tempRoot $asset
$checksums = Join-Path $tempRoot "SHA256SUMS.txt"
$unpackRoot = Join-Path $tempRoot "unpacked"
$installRoot = if ($env:VEX_INSTALL_DIR) { $env:VEX_INSTALL_DIR } else { Join-Path $env:LOCALAPPDATA "Programs\VEX" }
$binPath = Join-Path $installRoot "bin"

try {
    New-Item -ItemType Directory -Force -Path $unpackRoot | Out-Null

    Write-Host "Downloading VEX for Windows x64..."
    Invoke-WebRequest -Uri "$baseUrl/$asset" -OutFile $archive
    Invoke-WebRequest -Uri "$baseUrl/SHA256SUMS.txt" -OutFile $checksums

    $sumLine = Get-Content $checksums |
        Where-Object { $_ -match "\s+$([regex]::Escape($asset))$" } |
        Select-Object -First 1
    if (-not $sumLine) {
        throw "The release checksum file does not contain $asset"
    }
    $expectedHash = ($sumLine -split "\s+")[0].ToLowerInvariant()
    $actualHash = (Get-FileHash -Path $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $expectedHash) {
        throw "VEX archive checksum mismatch. Expected $expectedHash, got $actualHash"
    }

    Expand-Archive -Path $archive -DestinationPath $unpackRoot -Force
    $payload = Join-Path $unpackRoot "vex"
    if (-not (Test-Path (Join-Path $payload "bin\vex.exe"))) {
        throw "The downloaded archive does not contain the expected VEX executable"
    }

    if (Test-Path $installRoot) {
        if (-not (Test-Path (Join-Path $installRoot ".vex-managed"))) {
            throw "Refusing to replace $installRoot because it is not marked as a VEX-managed installation"
        }
        Remove-Item -Path $installRoot -Recurse -Force
    }

    New-Item -ItemType Directory -Force -Path (Split-Path $installRoot -Parent) | Out-Null
    Move-Item -Path $payload -Destination $installRoot
    New-Item -ItemType File -Path (Join-Path $installRoot ".vex-managed") | Out-Null

    $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
    $pathEntries = @($userPath -split ";" | Where-Object { $_ })
    $pathEntries = @($pathEntries | Where-Object {
        $_.TrimEnd("\") -ine $binPath.TrimEnd("\")
    })
    $pathEntries += $binPath
    [Environment]::SetEnvironmentVariable("Path", ($pathEntries -join ";"), "User")
    $env:Path = "$binPath;$env:Path"

    $vsix = Join-Path $installRoot "share\vex\vex-language-0.1.0.vsix"
    $codeCommand = Get-Command code -ErrorAction SilentlyContinue
    if ($env:VEX_SKIP_VSCODE) {
        # Installer integration tests intentionally skip editor modification.
    } elseif ($codeCommand -and (Test-Path $vsix)) {
        Write-Host "Installing the VEX extension in Visual Studio Code..."
        & $codeCommand.Source --install-extension $vsix --force
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "The VEX CLI is installed, but VS Code did not accept the extension. Install it manually from: $vsix"
        }
    } elseif (Test-Path $vsix) {
        Write-Host "VS Code's 'code' command was not found. The extension is available at: $vsix"
    }

    Write-Host ""
    Write-Host "VEX is installed at $installRoot"
    Write-Host "Open a new terminal, then run: vex --version"
    Write-Host "In VS Code, open a .vex file and use VEX: Run Current File."
} finally {
    if (Test-Path $tempRoot) {
        Remove-Item -Path $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
