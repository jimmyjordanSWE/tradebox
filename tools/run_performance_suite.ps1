param(
    [ValidateSet("iex", "sip")]
    [string]$Feed = "iex",
    [string]$BuildDirectory = "build",
    [ValidateRange(0, 20)]
    [int]$WarmupRuns = 2,
    [ValidateRange(1, 50)]
    [int]$MeasuredRuns = 9
)

$ErrorActionPreference = "Stop"

$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
$executable = Join-Path $build "tradebox_market_replay_benchmark.exe"
if (-not (Test-Path -LiteralPath $executable)) {
    throw "Benchmark executable not found: $executable"
}

if ($Feed -eq "sip") {
    $corpus = Join-Path $build "testdata\alpaca_sip_stream_20x25000.jsonl"
    $events = 500000
    $instruments = 20
} else {
    $corpus = Join-Path $build "testdata\alpaca_market_stream_10x10000.jsonl"
    $events = 100000
    $instruments = 10
}
if (-not (Test-Path -LiteralPath $corpus)) {
    throw "Benchmark corpus not found: $corpus"
}

$result = Join-Path $build "benchmark-results\manual-$Feed.json"
$ingestion = @()
$durable = @()

for ($run = 1; $run -le $WarmupRuns; ++$run) {
    & $executable $corpus $events $result $Feed $instruments | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Warm-up run $run failed"
    }
}

for ($run = 1; $run -le $MeasuredRuns; ++$run) {
    $line = & $executable $corpus $events $result $Feed $instruments |
        Select-Object -Last 1
    if ($LASTEXITCODE -ne 0) {
        throw "Measured run $run failed"
    }
    if ($line -notmatch
        "decode\+store\+enqueue ([0-9]+) events/s \| including durable flush ([0-9]+) events/s") {
        throw "Could not parse benchmark output: $line"
    }
    $ingestion += [double]$Matches[1]
    $durable += [double]$Matches[2]
    [pscustomobject]@{
        Run = $run
        IngestionEventsPerSecond = $ingestion[-1]
        DurableEventsPerSecond = $durable[-1]
    }
}

function Get-Summary([double[]]$Values) {
    $sorted = @($Values | Sort-Object)
    $mean = ($Values | Measure-Object -Average).Average
    $variance = (
        $Values |
            ForEach-Object { [math]::Pow($_ - $mean, 2) } |
            Measure-Object -Average
    ).Average
    if ($sorted.Count % 2 -eq 0) {
        $middle = $sorted.Count / 2
        $median = ($sorted[$middle - 1] + $sorted[$middle]) / 2
    } else {
        $median = $sorted[[math]::Floor($sorted.Count / 2)]
    }
    [pscustomobject]@{
        Minimum = [math]::Round($sorted[0])
        Median = [math]::Round($median)
        Mean = [math]::Round($mean)
        Maximum = [math]::Round($sorted[-1])
        StandardDeviation = [math]::Round([math]::Sqrt($variance))
    }
}

""
"$Feed summary ($MeasuredRuns measured runs after $WarmupRuns warm-ups)"
$ingestionSummary = Get-Summary $ingestion
$durableSummary = Get-Summary $durable
@(
    [pscustomobject]@{
        Metric = "Decode/store/enqueue"
        Minimum = $ingestionSummary.Minimum
        Median = $ingestionSummary.Median
        Mean = $ingestionSummary.Mean
        Maximum = $ingestionSummary.Maximum
        StandardDeviation = $ingestionSummary.StandardDeviation
    }
    [pscustomobject]@{
        Metric = "Including durable flush"
        Minimum = $durableSummary.Minimum
        Median = $durableSummary.Median
        Mean = $durableSummary.Mean
        Maximum = $durableSummary.Maximum
        StandardDeviation = $durableSummary.StandardDeviation
    }
) | Format-Table -AutoSize
