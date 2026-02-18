param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("begin", "finish")]
    [string]$Mode,

    [ValidateSet("feat", "fix", "refactor", "docs", "chore")]
    [string]$Type = "chore",

    [string]$Message = "",

    [string]$IdfInitScript = "C:\Espressif\Initialize-Idf.ps1",
    [string]$IdfId = "esp-idf-b29c58f93b4ca0f49cdfc4c3ef43b562",
    [string]$WikiRepoPath = "..\esp32-keyboard.wiki"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Run-Git {
    param(
        [string]$Repo,
        [string[]]$GitArgs
    )
    $output = & git -C $Repo @GitArgs 2>&1
    $code = $LASTEXITCODE
    return [pscustomobject]@{
        ExitCode = $code
        Output   = $output
    }
}

function Assert-GitOk {
    param(
        [string]$Repo,
        [string[]]$GitArgs,
        [string]$Context
    )
    $r = Run-Git -Repo $Repo -GitArgs $GitArgs
    if ($r.ExitCode -ne 0) {
        throw "$Context failed`n$($r.Output -join "`n")"
    }
    return $r.Output
}

function Get-DirtyEntries {
    param([string]$Repo)

    $lines = Assert-GitOk -Repo $Repo -GitArgs @("status", "--porcelain=v1") -Context "git status"
    $entries = @()

    foreach ($line in $lines) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        if ($line.Length -lt 4) {
            continue
        }
        $status = $line.Substring(0, 2)
        $pathPart = $line.Substring(3)
        $path = $pathPart
        if ($pathPart.Contains(" -> ")) {
            $path = $pathPart.Split(" -> ")[-1]
        }

        $fullPath = Join-Path $Repo $path
        $hash = ""
        if (Test-Path -LiteralPath $fullPath) {
            $hashOut = Assert-GitOk -Repo $Repo -GitArgs @("hash-object", "--", $path) -Context "git hash-object $path"
            $hash = ($hashOut | Select-Object -First 1).Trim()
        }

        $entries += [pscustomobject]@{
            path   = $path
            status = $status
            hash   = $hash
        }
    }

    return $entries
}

function Get-RepoSnapshot {
    param([string]$Repo)

    $branchOut = Assert-GitOk -Repo $Repo -GitArgs @("rev-parse", "--abbrev-ref", "HEAD") -Context "git rev-parse"
    if ($branchOut -is [array]) {
        $branch = ($branchOut | Select-Object -First 1).ToString().Trim()
    }
    else {
        $branch = $branchOut.ToString().Trim()
    }
    $remotes = Assert-GitOk -Repo $Repo -GitArgs @("remote", "-v") -Context "git remote -v"
    $dirty = Get-DirtyEntries -Repo $Repo

    return [pscustomobject]@{
        repo_path = (Resolve-Path $Repo).Path
        branch    = $branch
        remotes   = @($remotes)
        dirty     = @($dirty)
    }
}

function Build-DirtyMap {
    param([object[]]$Entries)
    $map = @{}
    foreach ($e in $Entries) {
        $map[$e.path] = $e
    }
    return $map
}

function Get-TouchedFilesSinceBaseline {
    param(
        [object]$BaselineRepo,
        [object]$CurrentRepo
    )

    $baselineMap = Build-DirtyMap -Entries $BaselineRepo.dirty
    $currentMap = Build-DirtyMap -Entries $CurrentRepo.dirty
    $touched = New-Object System.Collections.Generic.List[string]

    foreach ($path in $currentMap.Keys) {
        if (-not $baselineMap.ContainsKey($path)) {
            $touched.Add($path)
            continue
        }

        if ($baselineMap[$path].hash -ne $currentMap[$path].hash) {
            $touched.Add($path)
        }
    }

    return @($touched | Sort-Object -Unique)
}

function Push-WithOneRetry {
    param(
        [string]$Repo,
        [string]$Remote,
        [string]$Branch
    )

    $push1 = Run-Git -Repo $Repo -GitArgs @("push", $Remote, $Branch)
    if ($push1.ExitCode -eq 0) {
        return
    }

    Write-Warning "Initial push failed; trying pull --rebase once."
    $pull = Run-Git -Repo $Repo -GitArgs @("pull", "--rebase", $Remote, $Branch)
    if ($pull.ExitCode -ne 0) {
        throw "Push rejected and pull --rebase failed`n$($pull.Output -join "`n")"
    }

    $push2 = Run-Git -Repo $Repo -GitArgs @("push", $Remote, $Branch)
    if ($push2.ExitCode -ne 0) {
        throw "Push still failing after pull --rebase`n$($push2.Output -join "`n")"
    }
}

function Run-IdfBuild {
    param(
        [string]$Repo,
        [string]$InitScriptPath,
        [string]$InitId
    )

    if (-not (Test-Path -LiteralPath $InitScriptPath)) {
        throw "ESP-IDF init script not found: $InitScriptPath"
    }

    Push-Location $Repo
    try {
        . $InitScriptPath -IdfId $InitId
        & idf.py build
        if ($LASTEXITCODE -ne 0) {
            throw "idf.py build failed with exit code $LASTEXITCODE"
        }
    }
    finally {
        Pop-Location
    }
}

function Sync-WikiDocs {
    param(
        [string]$MainRepo,
        [string]$WikiRepo,
        [string[]]$TouchedMainFiles
    )

    $wikiCandidates = $TouchedMainFiles | Where-Object {
        $_ -like "docs/wiki/*.md"
    }

    if (-not $wikiCandidates -or $wikiCandidates.Count -eq 0) {
        Write-Host "No wiki-doc changes detected; skipping wiki sync."
        return $false
    }

    if (-not (Test-Path -LiteralPath $WikiRepo)) {
        throw "Wiki repo path not found: $WikiRepo"
    }

    foreach ($path in $wikiCandidates) {
        $src = Join-Path $MainRepo $path
        $destName = [System.IO.Path]::GetFileName($path)
        $dest = Join-Path $WikiRepo $destName

        if (Test-Path -LiteralPath $src) {
            Copy-Item -LiteralPath $src -Destination $dest -Force
        }
        else {
            if (Test-Path -LiteralPath $dest) {
                Remove-Item -LiteralPath $dest -Force
            }
        }
    }

    $wikiStatus = Assert-GitOk -Repo $WikiRepo -GitArgs @("status", "--porcelain=v1") -Context "wiki git status"
    if (-not $wikiStatus -or $wikiStatus.Count -eq 0) {
        Write-Host "Wiki repo unchanged after sync; skipping wiki commit."
        return $false
    }

    Assert-GitOk -Repo $WikiRepo -GitArgs @("add", "--all") -Context "wiki git add"
    return $true
}

$mainRepo = (Resolve-Path ".").Path
$wikiRepo = (Resolve-Path (Join-Path $mainRepo $WikiRepoPath)).Path
$baselineFile = Join-Path $mainRepo ".git\post_change_baseline.json"

if ($Mode -eq "begin") {
    $snapshot = [pscustomobject]@{
        started_at = [DateTime]::UtcNow.ToString("o")
        main       = Get-RepoSnapshot -Repo $mainRepo
        wiki       = Get-RepoSnapshot -Repo $wikiRepo
    }

    $snapshot | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $baselineFile -Encoding UTF8
    Write-Host "Baseline captured."
    Write-Host "Main branch: $($snapshot.main.branch)"
    Write-Host "Wiki branch: $($snapshot.wiki.branch)"
    Write-Host "Baseline file: $baselineFile"
    exit 0
}

if (-not (Test-Path -LiteralPath $baselineFile)) {
    throw "Baseline not found. Run begin mode first: .\tools\post_change_pipeline.ps1 -Mode begin"
}

if ([string]::IsNullOrWhiteSpace($Message)) {
    throw "Message is required in finish mode."
}

$baseline = Get-Content -LiteralPath $baselineFile -Raw | ConvertFrom-Json
$currentMain = Get-RepoSnapshot -Repo $mainRepo
$currentWiki = Get-RepoSnapshot -Repo $wikiRepo
$touchedMain = Get-TouchedFilesSinceBaseline -BaselineRepo $baseline.main -CurrentRepo $currentMain

if (-not $touchedMain -or $touchedMain.Count -eq 0) {
    Write-Host "No-op: no effective main-repo file changes since baseline."
    Remove-Item -LiteralPath $baselineFile -Force
    exit 0
}

Write-Host "Touched files since baseline:"
$touchedMain | ForEach-Object { Write-Host " - $_" }

Run-IdfBuild -Repo $mainRepo -InitScriptPath $IdfInitScript -InitId $IdfId

$addArgs = @("add", "--") + $touchedMain
Assert-GitOk -Repo $mainRepo -GitArgs $addArgs -Context "git add touched files"

$cachedDiff = Run-Git -Repo $mainRepo -GitArgs @("diff", "--cached", "--quiet")
if ($cachedDiff.ExitCode -eq 0) {
    Write-Host "No-op: nothing staged after touched-file filtering."
    Remove-Item -LiteralPath $baselineFile -Force
    exit 0
}

$commitMsg = "$Type`: $Message"
Assert-GitOk -Repo $mainRepo -GitArgs @("commit", "-m", $commitMsg) -Context "git commit"
Push-WithOneRetry -Repo $mainRepo -Remote "origin" -Branch $currentMain.branch

$wikiNeedsCommit = Sync-WikiDocs -MainRepo $mainRepo -WikiRepo $wikiRepo -TouchedMainFiles $touchedMain
if ($wikiNeedsCommit) {
    Assert-GitOk -Repo $wikiRepo -GitArgs @("commit", "-m", $commitMsg) -Context "wiki git commit"
    Push-WithOneRetry -Repo $wikiRepo -Remote "origin" -Branch $currentWiki.branch
}

Remove-Item -LiteralPath $baselineFile -Force
Write-Host "Pipeline finished successfully."
