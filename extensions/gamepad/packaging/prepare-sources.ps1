# SPDX-License-Identifier: GPL-3.0-only
param([switch]$Direct)
$ErrorActionPreference = 'Stop'
$pluginRoot = Split-Path $PSScriptRoot -Parent
$lock = Get-Content -LiteralPath (Join-Path $pluginRoot 'host-core/upstream.lock.json') -Raw | ConvertFrom-Json
$upstreamPath = Join-Path $pluginRoot 'host-core/upstream'
$gitOptions = @()
if ($Direct) { $gitOptions = @('-c', 'http.proxy=', '-c', 'https.proxy=') }
function Invoke-PinnedGit {
    param([string[]]$Arguments)
    & git @gitOptions @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Git failed: $($Arguments -join ' ')" }
}
if (-not (Test-Path -LiteralPath (Join-Path $upstreamPath '.git'))) {
    Invoke-PinnedGit @('clone', '--filter=blob:none', '--no-checkout', $lock.repository, $upstreamPath)
}
Invoke-PinnedGit @('-C', $upstreamPath, 'fetch', '--depth=1', 'origin', $lock.commit)
Invoke-PinnedGit @('-C', $upstreamPath, 'sparse-checkout', 'set', '--no-cone', '/src/', '/LICENSE', '/.gitmodules', '/third-party/moonlight-common-c/', '/third-party/Simple-Web-Server/')
Invoke-PinnedGit @('-C', $upstreamPath, 'checkout', '--detach', $lock.commit)
Invoke-PinnedGit @('-C', $upstreamPath, 'submodule', 'update', '--init', '--recursive', '--depth=1', 'third-party/moonlight-common-c', 'third-party/Simple-Web-Server')
& python (Join-Path $PSScriptRoot 'prepare_protocol.py')
if ($LASTEXITCODE -ne 0) { throw 'Could not materialize the pinned protocol fork.' }
