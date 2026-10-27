# SPDX-License-Identifier: GPL-3.0-only
param(
    [Parameter(Mandatory)][string]$Url,
    [Parameter(Mandatory)][string]$Destination,
    [Parameter(Mandatory)][string]$Sha512
)
$ErrorActionPreference = 'Stop'
if ($Sha512 -notmatch '^[0-9a-fA-F]{128}$') { throw 'An expected SHA512 digest is required.' }
$sourceUri = [Uri]$Url
if ($sourceUri.Scheme -ne 'https') { throw 'Build dependency downloads require HTTPS.' }
# GitHub's official codeload endpoint produces the same archive. The pinned
# digest below must still match; no mirror or certificate bypass is involved.
if ($sourceUri.Host -eq 'github.com' -and $sourceUri.AbsolutePath -match '^/([^/]+)/([^/]+)/archive/(.+)\.(tar\.gz|zip)$') {
    $owner = $Matches[1]
    $repository = $Matches[2]
    $reference = $Matches[3]
    $format = $Matches[4]
    if ($reference -notmatch '^(refs/|[0-9a-fA-F]{40}$)') { $reference = "refs/tags/$reference" }
    $Url = "https://codeload.github.com/$owner/$repository/$format/$reference"
}
$temporary = "$Destination.powershell.part"
$download = @{ Uri = $Url; OutFile = $temporary; ConnectionTimeoutSeconds = 15;
    OperationTimeoutSeconds = 180; UserAgent = 'CemuGamePad-build' }
if ($env:NO_PROXY -eq '*') { $download.NoProxy = $true }
else {
    $proxyAddress = $env:HTTPS_PROXY
    if (-not $proxyAddress) { $proxyAddress = $env:HTTP_PROXY }
    if ($proxyAddress) {
        if ($proxyAddress -notmatch '^[a-zA-Z][a-zA-Z0-9+.-]*://') { $proxyAddress = "http://$proxyAddress" }
        $download.Proxy = $proxyAddress
    }
}
try {
    for ($attempt = 1; $attempt -le 3; ++$attempt) {
        try {
            $request = $download.Clone()
            Invoke-WebRequest @request
            if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA512).Hash -ine $Sha512) {
                throw 'Downloaded dependency does not match the pinned SHA512 digest.'
            }
            Move-Item -LiteralPath $temporary -Destination $Destination -Force
            exit 0
        }
        catch {
            if ($attempt -eq 3) { throw }
            Write-Warning "Dependency download attempt $attempt failed: $($_.Exception.Message)"
        }
    }
}
finally {
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
}
