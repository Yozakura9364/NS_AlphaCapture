[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string] $TestedDll,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string] $Version,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9A-Fa-f]{64}$')]
    [string] $ExpectedDllHash,

    [string] $ReleaseRoot = '',

    [switch] $Deploy,

    [string[]] $Destination = @()
)

$ErrorActionPreference = 'Stop'
$utf8NoBom = New-Object System.Text.UTF8Encoding $false
if ([string]::IsNullOrWhiteSpace($ReleaseRoot)) {
    $cursor = Get-Item -LiteralPath $PSScriptRoot
    while ($null -ne $cursor) {
        $candidate = Join-Path $cursor.FullName 'release'
        if (Test-Path -LiteralPath $candidate -PathType Container) {
            $ReleaseRoot = $candidate
            break
        }
        $cursor = $cursor.Parent
    }
    if ([string]::IsNullOrWhiteSpace($ReleaseRoot)) {
        throw 'Cannot locate a release directory. Pass -ReleaseRoot explicitly.'
    }
}
$releaseRoot = [System.IO.Path]::GetFullPath((Resolve-Path -LiteralPath $ReleaseRoot).Path)
$testedDll = [System.IO.Path]::GetFullPath((Resolve-Path -LiteralPath $TestedDll).Path)
$expectedDllHash = $ExpectedDllHash.ToUpperInvariant()
$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$licenseSource = Join-Path $projectRoot 'LICENSE.txt'
$thirdPartyNoticesSource = Join-Path $projectRoot 'THIRD_PARTY_NOTICES.txt'

foreach ($noticePath in @($licenseSource, $thirdPartyNoticesSource)) {
    if (-not (Test-Path -LiteralPath $noticePath -PathType Leaf)) {
        throw "Required license file is missing: $noticePath"
    }
}

if ($Deploy -and $Destination.Count -eq 0) {
    throw '-Deploy requires at least one explicit -Destination path.'
}
if (-not $Deploy -and $Destination.Count -ne 0) {
    throw '-Destination requires -Deploy.'
}
foreach ($destinationPath in $Destination) {
    $destinationFull = [System.IO.Path]::GetFullPath($destinationPath)
    if ([System.IO.Path]::GetFileName($destinationFull) -ne 'NS_AlphaCapture.addon64') {
        throw "Destination must end with NS_AlphaCapture.addon64: $destinationFull"
    }
}

$requiredFiles = @(
    'reshade-addons\NS_AlphaCapture.ini',
    'reshade-shaders\NS\NS_AlphaBase.fx',
    'reshade-shaders\NS\NS_VFXCapture.fx'
)
foreach ($relativePath in $requiredFiles) {
    $path = Join-Path $releaseRoot $relativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required release file is missing: $relativePath"
    }
}

$sourceHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $testedDll).Hash.ToUpperInvariant()
if ($sourceHash -ne $expectedDllHash) {
    throw "Tested DLL hash mismatch. Expected $expectedDllHash, got $sourceHash."
}

$releaseAddon = Join-Path $releaseRoot 'NS_AlphaCapture.addon64'
$releaseNestedAddon = Join-Path $releaseRoot 'reshade-addons\NS_AlphaCapture.addon64'
$versionPath = Join-Path $releaseRoot 'VERSION.txt'
$licensePath = Join-Path $releaseRoot 'LICENSE.txt'
$thirdPartyNoticesPath = Join-Path $releaseRoot 'THIRD_PARTY_NOTICES.txt'
$zipPath = Join-Path $releaseRoot 'NS-AlphaCapture.zip'
$sumsPath = Join-Path $releaseRoot 'SHA256SUMS.txt'

if ($WhatIfPreference) {
    $null = $PSCmdlet.ShouldProcess($releaseAddon, 'copy the user-tested addon')
    $null = $PSCmdlet.ShouldProcess($versionPath, "write version $Version")
    $null = $PSCmdlet.ShouldProcess($licensePath, 'copy the project license')
    $null = $PSCmdlet.ShouldProcess($thirdPartyNoticesPath, 'copy third-party notices')
    $null = $PSCmdlet.ShouldProcess($zipPath, 'rebuild release ZIP')
    $null = $PSCmdlet.ShouldProcess($sumsPath, 'write release SHA256SUMS')
    if ($Deploy) {
        foreach ($destinationPath in $Destination) {
            $destinationFull = [System.IO.Path]::GetFullPath($destinationPath)
            $parent = Split-Path -Parent $destinationFull
            if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
                throw "Destination directory is missing: $parent"
            }
            $null = $PSCmdlet.ShouldProcess($destinationFull, 'deploy user-tested addon without touching INI')
        }
    }
    Write-Output "VALIDATED NS Alpha Capture $Version release plan"
    Write-Output "ADDON SHA256 $sourceHash"
    return
}

if ($PSCmdlet.ShouldProcess($releaseAddon, 'copy the user-tested addon')) {
    if (-not [string]::Equals($testedDll, $releaseAddon,
        [System.StringComparison]::OrdinalIgnoreCase)) {
        Copy-Item -LiteralPath $testedDll -Destination $releaseAddon -Force
    }
    if (-not [string]::Equals($testedDll, $releaseNestedAddon,
        [System.StringComparison]::OrdinalIgnoreCase)) {
        Copy-Item -LiteralPath $testedDll -Destination $releaseNestedAddon -Force
    }
}

$versionText = "NS Alpha Capture $Version`nVerified with ReShade 6.3.3.0 / FFXIV DX11 / addon API 14`n"
if ($PSCmdlet.ShouldProcess($versionPath, "write version $Version")) {
    [System.IO.File]::WriteAllText($versionPath, $versionText, $utf8NoBom)
}
if ($PSCmdlet.ShouldProcess($licensePath, 'copy the project license')) {
    Copy-Item -LiteralPath $licenseSource -Destination $licensePath -Force
}
if ($PSCmdlet.ShouldProcess($thirdPartyNoticesPath, 'copy third-party notices')) {
    Copy-Item -LiteralPath $thirdPartyNoticesSource -Destination $thirdPartyNoticesPath -Force
}

$tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$staging = Join-Path $tempRoot ('ns-alpha-release-' + [guid]::NewGuid().ToString('N'))
$stagingFull = [System.IO.Path]::GetFullPath($staging)
if (-not $stagingFull.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Unsafe staging path: $stagingFull"
}
$zipCandidate = $stagingFull + '.zip'

try {
    New-Item -ItemType Directory -Path (Join-Path $staging 'reshade-addons') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $staging 'reshade-shaders\NS') -Force | Out-Null
    $zipEntries = @(
        @{ Source = $releaseNestedAddon; Relative = 'reshade-addons\NS_AlphaCapture.addon64' },
        @{ Source = (Join-Path $releaseRoot 'reshade-addons\NS_AlphaCapture.ini'); Relative = 'reshade-addons\NS_AlphaCapture.ini' },
        @{ Source = (Join-Path $releaseRoot 'reshade-shaders\NS\NS_AlphaBase.fx'); Relative = 'reshade-shaders\NS\NS_AlphaBase.fx' },
        @{ Source = (Join-Path $releaseRoot 'reshade-shaders\NS\NS_VFXCapture.fx'); Relative = 'reshade-shaders\NS\NS_VFXCapture.fx' },
        @{ Source = $licensePath; Relative = 'LICENSE.txt' },
        @{ Source = $thirdPartyNoticesPath; Relative = 'THIRD_PARTY_NOTICES.txt' }
    )
    foreach ($entry in $zipEntries) {
        Copy-Item -LiteralPath $entry.Source -Destination (Join-Path $staging $entry.Relative)
    }

    if ($PSCmdlet.ShouldProcess($zipPath, 'rebuild release ZIP')) {
        Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $zipCandidate -CompressionLevel Optimal
        Copy-Item -LiteralPath $zipCandidate -Destination $zipPath -Force
    }
}
finally {
    if (Test-Path -LiteralPath $stagingFull) {
        Remove-Item -LiteralPath $stagingFull -Recurse -Force
    }
    if (Test-Path -LiteralPath $zipCandidate) {
        Remove-Item -LiteralPath $zipCandidate -Force
    }
}

$checksumEntries = @(
    @{ Path = $releaseNestedAddon; Name = 'NS_AlphaCapture.addon64' },
    @{ Path = (Join-Path $releaseRoot 'reshade-addons\NS_AlphaCapture.ini'); Name = 'NS_AlphaCapture.ini' },
    @{ Path = (Join-Path $releaseRoot 'reshade-shaders\NS\NS_AlphaBase.fx'); Name = 'NS_AlphaBase.fx' },
    @{ Path = (Join-Path $releaseRoot 'reshade-shaders\NS\NS_VFXCapture.fx'); Name = 'NS_VFXCapture.fx' },
    @{ Path = $licensePath; Name = 'LICENSE.txt' },
    @{ Path = $thirdPartyNoticesPath; Name = 'THIRD_PARTY_NOTICES.txt' },
    @{ Path = $zipPath; Name = 'NS-AlphaCapture.zip' }
)
$expectedZipEntries = @(
    'reshade-addons\NS_AlphaCapture.addon64',
    'reshade-addons\NS_AlphaCapture.ini',
    'reshade-shaders\NS\NS_AlphaBase.fx',
    'reshade-shaders\NS\NS_VFXCapture.fx',
    'LICENSE.txt',
    'THIRD_PARTY_NOTICES.txt'
)
if (Test-Path -LiteralPath $zipPath -PathType Leaf) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        $actualZipEntries = @($archive.Entries | ForEach-Object { $_.FullName })
        if (($actualZipEntries -join '|') -ne ($expectedZipEntries -join '|')) {
            throw "Release ZIP entries differ from the fixed six-file contract."
        }
        $zipDllEntry = $archive.GetEntry('reshade-addons\NS_AlphaCapture.addon64')
        if ($null -eq $zipDllEntry) {
            throw 'Release ZIP does not contain the addon.'
        }
        $sha256 = [System.Security.Cryptography.SHA256]::Create()
        try {
            $stream = $zipDllEntry.Open()
            try {
                $zipDllHash = ([System.BitConverter]::ToString($sha256.ComputeHash($stream))).Replace('-', '')
            }
            finally {
                $stream.Dispose()
            }
        }
        finally {
            $sha256.Dispose()
        }
        if ($zipDllHash -ne $expectedDllHash) {
            throw "Release ZIP addon hash mismatch: $zipDllHash"
        }
    }
    finally {
        $archive.Dispose()
    }
}
$checksumLines = foreach ($entry in $checksumEntries) {
    if (-not (Test-Path -LiteralPath $entry.Path -PathType Leaf)) {
        throw "Release output is missing: $($entry.Name)"
    }
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $entry.Path).Hash.ToUpperInvariant()
    "$hash  $($entry.Name)"
}
if ($PSCmdlet.ShouldProcess($sumsPath, 'write release SHA256SUMS')) {
    [System.IO.File]::WriteAllText($sumsPath, (($checksumLines -join "`n") + "`n"), $utf8NoBom)
}

if ($Deploy) {
    foreach ($destinationPath in $Destination) {
        $destinationFull = [System.IO.Path]::GetFullPath($destinationPath)
        $parent = Split-Path -Parent $destinationFull
        if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
            throw "Destination directory is missing: $parent"
        }
        $deployNow = $PSCmdlet.ShouldProcess($destinationFull, 'deploy user-tested addon without touching INI')
        if ($deployNow) {
            Copy-Item -LiteralPath $testedDll -Destination $destinationFull -Force
        } else {
            continue
        }
        $deployedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $destinationFull).Hash.ToUpperInvariant()
        if ($deployedHash -ne $expectedDllHash) {
            throw "Deployed DLL hash mismatch at ${destinationFull}: $deployedHash"
        }
        Write-Output "DEPLOYED $deployedHash $destinationFull"
    }
}

Write-Output "RELEASED NS Alpha Capture $Version"
Write-Output "ADDON SHA256 $sourceHash"
Write-Output ($checksumLines -join "`n")
