param(
  [string]$Command = "IRPROBE",
  [string]$HostAddress = "192.168.6.50",
  [int]$Port = 5000,
  [int]$TimeoutMs = 20000
)

$tcp = [System.Net.Sockets.TcpClient]::new()
$tcp.ReceiveTimeout = $TimeoutMs
$tcp.SendTimeout = $TimeoutMs

try {
  $tcp.Connect($HostAddress, $Port)
  $stream = $tcp.GetStream()
  $line = $Command.Trim() + "`r`n"
  $cmd = [Text.Encoding]::ASCII.GetBytes($line)
  [void]$stream.Write($cmd, 0, $cmd.Length)

  $buf = New-Object byte[] 512
  $len = $stream.Read($buf, 0, $buf.Length)
  if ($len -gt 0) {
    [Text.Encoding]::ASCII.GetString($buf, 0, $len)
  }
}
finally {
  $tcp.Close()
}
