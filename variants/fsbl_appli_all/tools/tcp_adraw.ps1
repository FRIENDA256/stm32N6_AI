param(
  [string]$HostAddress = "192.168.1.50",
  [int]$Port = 5000,
  [int]$TimeoutMs = 3000
)

$tcp = [System.Net.Sockets.TcpClient]::new()
$tcp.ReceiveTimeout = $TimeoutMs
$tcp.SendTimeout = $TimeoutMs

try {
  $tcp.Connect($HostAddress, $Port)
  $stream = $tcp.GetStream()
  $cmd = [Text.Encoding]::ASCII.GetBytes("ADRAW`r`n")
  [void]$stream.Write($cmd, 0, $cmd.Length)

  $buf = New-Object byte[] 256
  $len = $stream.Read($buf, 0, $buf.Length)
  if ($len -gt 0) {
    [Text.Encoding]::ASCII.GetString($buf, 0, $len)
  }
}
finally {
  $tcp.Close()
}
