# Clone/verify reference sources only. Does not install dependencies or run repository code.
param([switch]$CheckOnly)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$referenceRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot 'work\references'))
$manifest = Get-Content -LiteralPath (Join-Path $projectRoot 'config\references.lock.json') -Raw | ConvertFrom-Json

function Invoke-ReferenceGit {
    param([string[]]$GitArgs)
    & git @GitArgs
    if ($LASTEXITCODE -ne 0) { throw 'Reference Git operation failed; existing files were not reset.' }
}

foreach ($entry in $manifest.repositories) {
    if ($entry.name -notmatch '^[A-Za-z0-9_-]+$' -or $entry.commit -notmatch '^[a-f0-9]{40}$') { throw 'Invalid reference manifest entry' }
    $referencePath = [IO.Path]::GetFullPath((Join-Path $referenceRoot $entry.name))
    if (-not $referencePath.StartsWith($referenceRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Reference escaped work directory' }
    if (-not (Test-Path -LiteralPath $referencePath)) {
        if ($CheckOnly) { throw ('Missing reference: ' + $entry.name) }
        New-Item -ItemType Directory -Path $referenceRoot -Force | Out-Null
        Invoke-ReferenceGit -GitArgs @('clone','--depth','1','--filter=blob:none','--sparse','--branch',$entry.branch,$entry.url,$referencePath)
        $initialHead = (& git -C $referencePath rev-parse HEAD).Trim()
        if ($initialHead -ne $entry.commit) {
            Invoke-ReferenceGit -GitArgs @('-C',$referencePath,'fetch','--depth','1','origin',$entry.commit)
            Invoke-ReferenceGit -GitArgs @('-C',$referencePath,'checkout','--detach',$entry.commit)
        }
        Invoke-ReferenceGit -GitArgs (@('-C',$referencePath,'sparse-checkout','set') + @($entry.sparse_paths))
    }
    if (-not (Test-Path -LiteralPath (Join-Path $referencePath '.git'))) { throw ('Not a reference checkout: ' + $entry.name) }
    $remote = & git -C $referencePath remote get-url origin
    if ($LASTEXITCODE -ne 0 -or $remote.Trim() -ne $entry.url) { throw ('Reference remote mismatch: ' + $entry.name) }
    $commit = & git -C $referencePath rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $commit.Trim() -ne $entry.commit) { throw ('Reference commit mismatch (not reset): ' + $entry.name) }
    $dirty = & git -C $referencePath status --porcelain
    if ($LASTEXITCODE -ne 0 -or $dirty) { throw ('Reference has local modifications (preserved): ' + $entry.name) }
    Write-Output ($entry.name + '  ' + $commit.Trim())
}
