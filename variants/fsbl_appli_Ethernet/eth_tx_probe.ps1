<#
.SYNOPSIS
  Test whether the STM32N6 board's Ethernet TX frames actually reach this PC.

.DESCRIPTION
  Use together with the raw-TX firmware (the build that broadcasts 0x88B5 frames
  and prints "MMC TX good" climbing on UART). Connect the board to this PC's
  wired NIC (direct, or via a switch) and run this script while the board sends.

  Two modes:
    Stats   (default) : polls the NIC RX counters once per second. No admin needed.
                        ReceivedBroadcast / ReceivedBytes climbing => board TX
                        physically reaches the PC.
    Capture           : uses Windows built-in pktmon to capture frames filtered by
                        the board's source MAC, then reports how many were seen and
                        dumps the decoded text. Requires an elevated PowerShell.

.EXAMPLE
  # Simple counter monitor for 30 s (run, then power the board):
  powershell -ExecutionPolicy Bypass -File .\eth_tx_probe.ps1

.EXAMPLE
  # Frame-level capture (Run PowerShell as Administrator):
  powershell -ExecutionPolicy Bypass -File .\eth_tx_probe.ps1 -Mode Capture -Seconds 20

.NOTES
  Board raw-TX identifiers: src MAC 02-00-00-00-00-01, EtherType 0x88B5,
  dst ff-ff-ff-ff-ff-ff, payload "STM32N6 RAW ETH BROADCAST SEQ=...".
#>
param(
    [string]$Nic = "Realtek PCIe GbE Family Controller",   # -InterfaceDescription of the wired NIC
    [ValidateSet("Stats", "Capture")][string]$Mode = "Stats",
    [int]$Seconds = 30,
    [string]$BoardMac = "02-00-00-00-00-01"
)

function Get-TargetAdapter {
    param([string]$Desc)
    $a = Get-NetAdapter -InterfaceDescription $Desc -ErrorAction SilentlyContinue
    if ($a) { return $a }
    Write-Host "NIC '$Desc' not found; auto-picking a connected physical wired NIC..." -ForegroundColor DarkYellow
    return Get-NetAdapter -Physical -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Status -eq 'Up' -and
            $_.InterfaceDescription -notmatch 'Virtual|VMware|Hyper-V|TAP|VPN|Bluetooth|Wi-?Fi|Wireless|Loopback'
        } | Select-Object -First 1
}

$adapter = Get-TargetAdapter -Desc $Nic
if (-not $adapter) {
    Write-Host "No target NIC found. Pass -Nic '<InterfaceDescription>' (see 'Get-NetAdapter')." -ForegroundColor Red
    exit 1
}

Write-Host ("Target NIC : {0}" -f $adapter.Name)
Write-Host ("Description: {0}" -f $adapter.InterfaceDescription)
Write-Host ("Link       : {0}  {1}  {2}" -f $adapter.Status, $adapter.LinkSpeed, $adapter.MediaConnectionState)
if ($adapter.Status -ne 'Up') {
    Write-Host "WARNING: NIC is not 'Up' - check cable / board link before trusting results." -ForegroundColor Yellow
}
Write-Host ""

if ($Mode -eq 'Stats') {
    $desc = $adapter.InterfaceDescription
    Write-Host "== RX counter monitor for $Seconds s. Power/flash the raw-TX (0x88B5) firmware NOW. =="
    Write-Host "   Watch dBcast / dBytes. Ctrl+C to stop early.`n"

    $base = Get-NetAdapterStatistics -InterfaceDescription $desc
    $prev = $base
    Write-Host ("{0,-6} {1,10} {2,10} {3,10} {4,12}" -f "t(s)", "dBcast", "dUcast", "dMcast", "dBytes")
    Write-Host ("{0,-6} {1,10} {2,10} {3,10} {4,12}" -f "----", "------", "------", "------", "------")

    for ($t = 1; $t -le $Seconds; $t++) {
        Start-Sleep -Seconds 1
        $cur = Get-NetAdapterStatistics -InterfaceDescription $desc
        $dB = $cur.ReceivedBroadcastPackets - $prev.ReceivedBroadcastPackets
        $dU = $cur.ReceivedUnicastPackets - $prev.ReceivedUnicastPackets
        $dM = $cur.ReceivedMulticastPackets - $prev.ReceivedMulticastPackets
        $dBy = $cur.ReceivedBytes - $prev.ReceivedBytes
        $line = ("{0,-6} {1,10} {2,10} {3,10} {4,12}" -f $t, $dB, $dU, $dM, $dBy)
        if ($dB -gt 0 -or $dBy -gt 0) { Write-Host $line -ForegroundColor Green } else { Write-Host $line }
        $prev = $cur
    }

    $totBcast = $prev.ReceivedBroadcastPackets - $base.ReceivedBroadcastPackets
    $totUcast = $prev.ReceivedUnicastPackets - $base.ReceivedUnicastPackets
    $totBytes = $prev.ReceivedBytes - $base.ReceivedBytes
    Write-Host ""
    Write-Host ("Total over {0}s : broadcast={1}  unicast={2}  bytes={3}" -f $Seconds, $totBcast, $totUcast, $totBytes)
    if ($totBcast -gt 0 -or $totBytes -gt 0) {
        Write-Host "VERDICT: RX traffic seen -> board TX reaches the PC. TX path is OK." -ForegroundColor Green
        Write-Host "         => Fault is the RX half only (RGMII PHY->MAC: PF7/PF10/PF14/PF15/PF8/PF9)." -ForegroundColor Green
    }
    else {
        Write-Host "VERDICT: ZERO frames received -> board TX does NOT reach the PC." -ForegroundColor Yellow
        Write-Host "         => TX-on-wire problem too (RGMII TX pins / GTX clock / PHY). Confirm UART 'MMC TX good' was climbing." -ForegroundColor Yellow
    }
}
elseif ($Mode -eq 'Capture') {
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $isAdmin) {
        Write-Host "Capture mode needs an ELEVATED PowerShell (Run as Administrator)." -ForegroundColor Red
        exit 1
    }

    $etl = Join-Path $env:TEMP "eth_tx_probe.etl"
    $txt = Join-Path $env:TEMP "eth_tx_probe.txt"
    if (Test-Path $etl) { Remove-Item $etl -Force }
    if (Test-Path $txt) { Remove-Item $txt -Force }

    Write-Host "Resetting pktmon filters..."
    pktmon filter remove | Out-Null
    # Filter to the board's source MAC so only its frames are captured.
    pktmon filter add BoardTx -m $BoardMac | Out-Null

    Write-Host ("Capturing {0}s (filtered to MAC {1}). Power/flash the raw-TX firmware NOW..." -f $Seconds, $BoardMac)
    pktmon start --capture --pkt-size 0 --file-name $etl | Out-Null
    Start-Sleep -Seconds $Seconds
    pktmon stop | Out-Null
    pktmon filter remove | Out-Null

    if (-not (Test-Path $etl)) {
        Write-Host "pktmon produced no capture file. Check pktmon availability / Windows version." -ForegroundColor Red
        exit 1
    }
    pktmon etl2txt $etl -o $txt | Out-Null

    $pkts = @()
    if (Test-Path $txt) {
        $pkts = Select-String -Path $txt -Pattern ('{0}|88B5|88 B5|STM32N6' -f ($BoardMac -replace '-', '.')) -AllMatches
    }
    Write-Host ""
    if ($pkts -and $pkts.Count -gt 0) {
        Write-Host ("VERDICT: {0} matching lines -> board frames captured. TX reaches the PC." -f $pkts.Count) -ForegroundColor Green
        Write-Host "First matches:"
        $pkts | Select-Object -First 8 | ForEach-Object { "  " + $_.Line.Trim() }
    }
    else {
        Write-Host "VERDICT: no board frames captured -> TX not reaching PC, or wrong NIC/filter." -ForegroundColor Yellow
        Write-Host "Tip: confirm UART 'MMC TX good' was climbing, and that the board is on THIS NIC's link."
    }
    Write-Host ("Raw capture text: {0}" -f $txt)
}
