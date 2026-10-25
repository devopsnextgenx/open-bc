# Registers the Windows runner, runs it, and tries to deregister when it exits.
$ErrorActionPreference = 'Stop'

if (-not $env:REPO_URL -or -not $env:REG_TOKEN) {
    Write-Host 'Error: REPO_URL and REG_TOKEN must be provided.'
    exit 1
}
$name = if ($env:RUNNER_NAME) { $env:RUNNER_NAME } else { $env:COMPUTERNAME }

Set-Location C:\actions-runner
# A restarted container keeps its old config; clear it so config.cmd does not refuse.
Remove-Item .runner, .credentials, .credentials_rsaparams -ErrorAction SilentlyContinue

Write-Host "Configuring runner '$name' for $($env:REPO_URL) (labels: $($env:RUNNER_LABELS))..."
$cfg = @('--url', $env:REPO_URL, '--token', $env:REG_TOKEN, '--name', $name, '--unattended', '--replace')
if ($env:RUNNER_LABELS) { $cfg += @('--labels', $env:RUNNER_LABELS) }
& .\config.cmd @cfg
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$code = 0
try {
    Write-Host 'Starting the GitHub Actions runner...'
    & .\run.cmd
    $code = $LASTEXITCODE
}
finally {
    # Best effort: Windows containers often get killed before this runs. A stale entry is replaced
    # by the next start (--replace) or can be deleted under Settings > Actions > Runners.
    Write-Host 'Removing runner from GitHub...'
    & .\config.cmd remove --token $env:REG_TOKEN
}
exit $code