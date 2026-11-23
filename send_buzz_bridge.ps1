param(
    [string]$PS5_HOST = "192.168.1.20",
    [int]$PS5_PORT = 9021
)

$elf = Join-Path $PSScriptRoot "buzz_bridge.elf"
if (-not (Test-Path $elf)) {
    Write-Error "buzz_bridge.elf niet gevonden!"
    exit 1
}

$bytes = [System.IO.File]::ReadAllBytes($elf)
$client = New-Object System.Net.Sockets.TcpClient
$iar = $client.BeginConnect($PS5_HOST, $PS5_PORT, $null, $null)
if (-not ($iar.AsyncWaitHandle.WaitOne(3000, $false) -and $client.Connected)) {
    $client.Close()
    Write-Error "Kan geen verbinding maken met elfldr op ${PS5_HOST}:${PS5_PORT}"
    exit 1
}
$stream = $client.GetStream()
$stream.Write($bytes, 0, $bytes.Length)
$stream.Flush()
$client.Close()
Write-Host "[+] buzz_bridge.elf ($($bytes.Length) bytes) verzonden naar ${PS5_HOST}:${PS5_PORT}" -ForegroundColor Green
