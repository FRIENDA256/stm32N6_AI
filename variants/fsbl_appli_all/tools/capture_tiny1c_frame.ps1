param(
  [string]$ComPort = "",
  [int]$BaudRate = 115200,
  [ValidateSet("IRCAPIMG", "IRCAPTEMP", "IRDUMP")]
  [string]$Command = "IRCAPIMG",
  [ValidateSet("auto", "image", "temp")]
  [string]$FrameKind = "auto",
  [ValidateSet("auto", "even", "odd")]
  [string]$ImageLane = "auto",
  [string]$InputRaw = "",
  [string]$HostAddress = "192.168.6.50",
  [int]$TcpPort = 5000,
  [int]$TimeoutMs = 90000,
  [string]$OutputDir = ".\tools\captures",
  [switch]$NoTcpTrigger,
  [switch]$Invert
)

function Find-BytePattern {
  param(
    [byte[]]$Buffer,
    [byte[]]$Pattern,
    [int]$Start = 0
  )

  if (($null -eq $Buffer) -or ($null -eq $Pattern) -or ($Pattern.Length -eq 0) -or ($Buffer.Length -lt $Pattern.Length)) {
    return -1
  }

  for ($i = $Start; $i -le ($Buffer.Length - $Pattern.Length); $i++) {
    $matched = $true
    for ($j = 0; $j -lt $Pattern.Length; $j++) {
      if ($Buffer[$i + $j] -ne $Pattern[$j]) {
        $matched = $false
        break
      }
    }
    if ($matched) {
      return $i
    }
  }

  return -1
}

function Read-SerialIntoBuffer {
  param(
    [System.IO.Ports.SerialPort]$Serial,
    [System.Collections.Generic.List[byte]]$Rx,
    [byte[]]$Scratch
  )

  try {
    $count = $Serial.Read($Scratch, 0, $Scratch.Length)
  }
  catch [System.TimeoutException] {
    return 0
  }

  for ($i = 0; $i -lt $count; $i++) {
    $Rx.Add($Scratch[$i])
  }

  return $count
}

function Read-TcpAvailableText {
  param([System.Net.Sockets.NetworkStream]$Stream)

  if ($null -eq $Stream) {
    return ""
  }

  $text = ""
  $buf = New-Object byte[] 512
  while ($Stream.DataAvailable) {
    $len = $Stream.Read($buf, 0, $buf.Length)
    if ($len -le 0) {
      break
    }
    $text += [Text.Encoding]::ASCII.GetString($buf, 0, $len)
  }

  return $text
}

function Get-AsciiTail {
  param(
    [byte[]]$Buffer,
    [int]$MaxLen = 512
  )

  if (($null -eq $Buffer) -or ($Buffer.Length -eq 0)) {
    return ""
  }

  $start = [Math]::Max(0, $Buffer.Length - $MaxLen)
  $len = $Buffer.Length - $start
  $tail = New-Object byte[] $len
  [Array]::Copy($Buffer, $start, $tail, 0, $len)
  return ([Text.Encoding]::ASCII.GetString($tail) -replace '[^\x09\x0A\x0D\x20-\x7E]', '.')
}

function Get-Crc32 {
  param([byte[]]$Data)

  [uint64]$crc = 4294967295
  foreach ($byte in $Data) {
    $crc = ($crc -bxor [uint64]$byte) -band 4294967295
    for ($bit = 0; $bit -lt 8; $bit++) {
      if (($crc -band 1) -ne 0) {
        $crc = (($crc -shr 1) -bxor 3988292384) -band 4294967295
      }
      else {
        $crc = ($crc -shr 1) -band 4294967295
      }
    }
  }

  return [uint32](($crc -bxor 4294967295) -band 4294967295)
}

function Convert-FrameToU16 {
  param([byte[]]$Frame)

  $pixelCount = [int]($Frame.Length / 2)
  $pixels = New-Object 'System.UInt16[]' $pixelCount
  for ($i = 0; $i -lt $pixelCount; $i++) {
    $pixels[$i] = [BitConverter]::ToUInt16($Frame, $i * 2)
  }

  return $pixels
}

function Convert-U16ToGray8 {
  param(
    [UInt16[]]$Pixels,
    [switch]$Invert
  )

  [uint32]$min = 65535
  [uint32]$max = 0
  [uint32]$nonzero = 0

  foreach ($pixel in $Pixels) {
    if ($pixel -ne 0) {
      $nonzero++
      if ($pixel -lt $min) { $min = $pixel }
      if ($pixel -gt $max) { $max = $pixel }
    }
  }

  if ($nonzero -eq 0) {
    $min = 0
    $max = 65535
  }

  $span = [int]($max - $min)
  if ($span -le 0) {
    $span = 1
  }

  $gray = New-Object byte[] $Pixels.Length
  for ($i = 0; $i -lt $Pixels.Length; $i++) {
    $value = [int]$Pixels[$i]
    if ($value -le $min) {
      $scaled = 0
    }
    elseif ($value -ge $max) {
      $scaled = 255
    }
    else {
      $scaled = [int][Math]::Round((($value - [int]$min) * 255.0) / $span)
    }

    if ($Invert) {
      $scaled = 255 - $scaled
    }
    $gray[$i] = [byte]$scaled
  }

  return @{
    Bytes = $gray
    Min = $min
    Max = $max
    NonZero = $nonzero
  }
}

function Get-ImageLaneScore {
  param(
    [byte[]]$Frame,
    [int]$FrameOffset,
    [int]$Width
  )

  $score = 0
  for ($x = 0; $x -lt $Width; $x++) {
    $value = [int]$Frame[$FrameOffset + ($x * 2)]
    $score += [Math]::Abs($value - 128)
  }

  return $score
}

function Convert-ImageFrameToGray8 {
  param(
    [byte[]]$Frame,
    [int]$Width,
    [int]$Height,
    [string]$Lane
  )

  $pixelCount = $Width * $Height
  $gray = New-Object byte[] $pixelCount

  for ($y = 0; $y -lt $Height; $y++) {
    $rowPixelOffset = $y * $Width
    $rowByteOffset = $rowPixelOffset * 2
    $laneOffset = 0

    if ($Lane -eq "odd") {
      $laneOffset = 1
    }
    elseif ($Lane -eq "auto") {
      $evenScore = Get-ImageLaneScore -Frame $Frame -FrameOffset $rowByteOffset -Width $Width
      $oddScore = Get-ImageLaneScore -Frame $Frame -FrameOffset ($rowByteOffset + 1) -Width $Width
      $laneOffset = if ($oddScore -gt $evenScore) { 1 } else { 0 }
    }

    for ($x = 0; $x -lt $Width; $x++) {
      $gray[$rowPixelOffset + $x] = $Frame[$rowByteOffset + ($x * 2) + $laneOffset]
    }
  }

  return $gray
}

function Write-Pgm16 {
  param(
    [string]$Path,
    [UInt16[]]$Pixels,
    [int]$Width,
    [int]$Height
  )

  $header = [Text.Encoding]::ASCII.GetBytes("P5`n$Width $Height`n65535`n")
  $stream = [System.IO.MemoryStream]::new()
  try {
    $stream.Write($header, 0, $header.Length)
    foreach ($pixel in $Pixels) {
      $stream.WriteByte([byte](($pixel -shr 8) -band 0xFF))
      $stream.WriteByte([byte]($pixel -band 0xFF))
    }
    [System.IO.File]::WriteAllBytes($Path, $stream.ToArray())
  }
  finally {
    $stream.Dispose()
  }
}

function Write-Pgm8 {
  param(
    [string]$Path,
    [byte[]]$Gray,
    [int]$Width,
    [int]$Height
  )

  $header = [Text.Encoding]::ASCII.GetBytes("P5`n$Width $Height`n255`n")
  $stream = [System.IO.MemoryStream]::new()
  try {
    $stream.Write($header, 0, $header.Length)
    $stream.Write($Gray, 0, $Gray.Length)
    [System.IO.File]::WriteAllBytes($Path, $stream.ToArray())
  }
  finally {
    $stream.Dispose()
  }
}

function Write-Bmp8As24 {
  param(
    [string]$Path,
    [byte[]]$Gray,
    [int]$Width,
    [int]$Height
  )

  $rowStride = (($Width * 3 + 3) -band (-bnot 3))
  $imageSize = $rowStride * $Height
  $fileSize = 54 + $imageSize
  $padding = New-Object byte[] ($rowStride - ($Width * 3))
  $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
  $writer = [System.IO.BinaryWriter]::new($stream)

  try {
    $writer.Write([byte[]](0x42, 0x4D))
    $writer.Write([uint32]$fileSize)
    $writer.Write([uint16]0)
    $writer.Write([uint16]0)
    $writer.Write([uint32]54)
    $writer.Write([uint32]40)
    $writer.Write([int32]$Width)
    $writer.Write([int32](-$Height))
    $writer.Write([uint16]1)
    $writer.Write([uint16]24)
    $writer.Write([uint32]0)
    $writer.Write([uint32]$imageSize)
    $writer.Write([int32]2835)
    $writer.Write([int32]2835)
    $writer.Write([uint32]0)
    $writer.Write([uint32]0)

    for ($y = 0; $y -lt $Height; $y++) {
      $rowOffset = $y * $Width
      for ($x = 0; $x -lt $Width; $x++) {
        $v = $Gray[$rowOffset + $x]
        $writer.Write([byte]$v)
        $writer.Write([byte]$v)
        $writer.Write([byte]$v)
      }
      if ($padding.Length -gt 0) {
        $writer.Write($padding)
      }
    }
  }
  finally {
    $writer.Dispose()
    $stream.Dispose()
  }
}

function Write-BmpGray8Palette {
  param(
    [string]$Path,
    [byte[]]$Gray,
    [int]$Width,
    [int]$Height
  )

  $rowStride = (($Width + 3) -band (-bnot 3))
  $imageSize = $rowStride * $Height
  $paletteSize = 256 * 4
  $pixelOffset = 14 + 40 + $paletteSize
  $fileSize = $pixelOffset + $imageSize
  $padding = New-Object byte[] ($rowStride - $Width)
  $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
  $writer = [System.IO.BinaryWriter]::new($stream)

  try {
    $writer.Write([byte[]](0x42, 0x4D))
    $writer.Write([uint32]$fileSize)
    $writer.Write([uint16]0)
    $writer.Write([uint16]0)
    $writer.Write([uint32]$pixelOffset)
    $writer.Write([uint32]40)
    $writer.Write([int32]$Width)
    $writer.Write([int32]$Height)
    $writer.Write([uint16]1)
    $writer.Write([uint16]8)
    $writer.Write([uint32]0)
    $writer.Write([uint32]$imageSize)
    $writer.Write([int32]2835)
    $writer.Write([int32]2835)
    $writer.Write([uint32]256)
    $writer.Write([uint32]0)

    for ($i = 0; $i -lt 256; $i++) {
      $writer.Write([byte]$i)
      $writer.Write([byte]$i)
      $writer.Write([byte]$i)
      $writer.Write([byte]0)
    }

    for ($y = $Height - 1; $y -ge 0; $y--) {
      $rowOffset = $y * $Width
      $writer.Write($Gray, $rowOffset, $Width)
      if ($padding.Length -gt 0) {
        $writer.Write($padding)
      }
    }
  }
  finally {
    $writer.Dispose()
    $stream.Dispose()
  }
}

function Resolve-FrameKind {
  param(
    [string]$RequestedKind,
    [string]$CommandName,
    [string]$InputPath,
    [string]$CmdHex
  )

  if ($RequestedKind -ne "auto") {
    return $RequestedKind
  }

  if ($CmdHex -eq "CC") {
    return "temp"
  }
  if ($CmdHex -eq "AA") {
    return "image"
  }
  if ($CommandName -eq "IRCAPTEMP") {
    return "temp"
  }
  if (($InputPath -match '(?i)temp') -and ($InputPath -notmatch '(?i)image')) {
    return "temp"
  }

  return "image"
}

function Save-Tiny1CFrameFiles {
  param(
    [byte[]]$Frame,
    [string]$Kind,
    [string]$OutputDir,
    [string]$Stamp,
    [int]$Width,
    [int]$Height,
    [string]$ImageLane,
    [switch]$Invert
  )

  $baseName = "tiny1c_${Kind}_${Stamp}"
  $rawPath = Join-Path $OutputDir ($baseName + ".raw")
  $pgm16Path = Join-Path $OutputDir ($baseName + "_16bit.pgm")
  $pgm8Path = Join-Path $OutputDir ($baseName + "_8bit.pgm")
  $bmpPath = Join-Path $OutputDir ($baseName + "_8bit.bmp")
  $u16BmpPath = Join-Path $OutputDir ($baseName + "_u16_auto.bmp")

  [System.IO.File]::WriteAllBytes($rawPath, $Frame)
  $pixels = Convert-FrameToU16 -Frame $Frame
  Write-Pgm16 -Path $pgm16Path -Pixels $pixels -Width $Width -Height $Height

  if ($Kind -eq "image") {
    $gray = Convert-ImageFrameToGray8 -Frame $Frame -Width $Width -Height $Height -Lane $ImageLane
    if ($Invert) {
      for ($i = 0; $i -lt $gray.Length; $i++) {
        $gray[$i] = [byte](255 - $gray[$i])
      }
    }
    Write-Pgm8 -Path $pgm8Path -Gray $gray -Width $Width -Height $Height
    Write-BmpGray8Palette -Path $bmpPath -Gray $gray -Width $Width -Height $Height

    $u16Info = Convert-U16ToGray8 -Pixels $pixels
    Write-Bmp8As24 -Path $u16BmpPath -Gray $u16Info.Bytes -Width $Width -Height $Height

    return @{
      Raw = $rawPath
      Pgm16 = $pgm16Path
      Pgm8 = $pgm8Path
      Bmp = $bmpPath
      U16Bmp = $u16BmpPath
      NonZero = ($gray | Where-Object { $_ -ne 0 }).Count
      Min = ($gray | Measure-Object -Minimum).Minimum
      Max = ($gray | Measure-Object -Maximum).Maximum
      Mode = "image y-only, lane=$ImageLane"
    }
  }

  $grayInfo = Convert-U16ToGray8 -Pixels $pixels -Invert:$Invert
  Write-Pgm8 -Path $pgm8Path -Gray $grayInfo.Bytes -Width $Width -Height $Height
  Write-BmpGray8Palette -Path $bmpPath -Gray $grayInfo.Bytes -Width $Width -Height $Height

  return @{
    Raw = $rawPath
    Pgm16 = $pgm16Path
    Pgm8 = $pgm8Path
    Bmp = $bmpPath
    U16Bmp = ""
    NonZero = $grayInfo.NonZero
    Min = $grayInfo.Min
    Max = $grayInfo.Max
    Mode = "temp/u16 auto stretch"
  }
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

if (-not [string]::IsNullOrWhiteSpace($InputRaw)) {
  $rawPath = (Resolve-Path -LiteralPath $InputRaw).Path
  $frame = [System.IO.File]::ReadAllBytes($rawPath)
  if ($frame.Length -ne 98304) {
    throw "InputRaw must be exactly 98304 bytes, got $($frame.Length): $rawPath"
  }

  $kind = Resolve-FrameKind -RequestedKind $FrameKind -CommandName $Command -InputPath $rawPath -CmdHex ""
  $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
  $saved = Save-Tiny1CFrameFiles -Frame $frame -Kind $kind -OutputDir $OutputDir -Stamp $stamp -Width 256 -Height 192 -ImageLane $ImageLane -Invert:$Invert
  Write-Host "Offline convert: $rawPath"
  Write-Host "Display mode: $($saved.Mode)"
  Write-Host "Pixel stats: nonzero=$($saved.NonZero) min=$($saved.Min) max=$($saved.Max)"
  Write-Host "Saved raw : $($saved.Raw)"
  Write-Host "Saved pgm : $($saved.Pgm16)"
  Write-Host "Saved pgm : $($saved.Pgm8)"
  Write-Host "Saved bmp : $($saved.Bmp)"
  if ($saved.U16Bmp -ne "") {
    Write-Host "Saved debug bmp: $($saved.U16Bmp)"
  }
  return
}

if ([string]::IsNullOrWhiteSpace($ComPort)) {
  $ports = [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object
  throw "Please specify -ComPort. Available ports: $($ports -join ', ')"
}

$beginPattern = [Text.Encoding]::ASCII.GetBytes("BEGIN_TINY1C_BINARY`r`n")
$endPattern = [Text.Encoding]::ASCII.GetBytes("END_TINY1C_BINARY")
$headerRegex = 'TINY1C_BIN V1 CMD=0x([0-9A-Fa-f]{2}) WIDTH=([0-9]+) HEIGHT=([0-9]+) BYTES=([0-9]+) SOURCE=([^ ]+) CRC32=0x([0-9A-Fa-f]{8})'

$serial = [System.IO.Ports.SerialPort]::new($ComPort, $BaudRate, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$serial.ReadTimeout = 200
$serial.WriteTimeout = 1000
$tcp = $null
$stream = $null

try {
  Write-Host "Opening serial $ComPort @ $BaudRate..."
  $serial.Open()
  Start-Sleep -Milliseconds 100
  $serial.DiscardInBuffer()

  if (-not $NoTcpTrigger) {
    Write-Host "Sending TCP command $Command to $HostAddress`:$TcpPort..."
    $tcp = [System.Net.Sockets.TcpClient]::new()
    $tcp.ReceiveTimeout = 5000
    $tcp.SendTimeout = 5000
    $tcp.Connect($HostAddress, $TcpPort)
    $stream = $tcp.GetStream()
    $cmdBytes = [Text.Encoding]::ASCII.GetBytes($Command + "`r`n")
    [void]$stream.Write($cmdBytes, 0, $cmdBytes.Length)
  }
  else {
    Write-Host "Waiting for an already-triggered Tiny1C binary frame..."
  }

  $rx = [System.Collections.Generic.List[byte]]::new()
  $scratch = New-Object byte[] 4096
  $watch = [System.Diagnostics.Stopwatch]::StartNew()
  $headerMatch = $null
  $tcpText = ""

  while ($true) {
    if ($watch.ElapsedMilliseconds -gt $TimeoutMs) {
      $serialTail = Get-AsciiTail -Buffer $rx.ToArray()
      throw "Timed out waiting for BEGIN_TINY1C_BINARY. TCP='$($tcpText.Trim())' SerialTail='$serialTail'"
    }

    [void](Read-SerialIntoBuffer -Serial $serial -Rx $rx -Scratch $scratch)
    $newTcpText = Read-TcpAvailableText -Stream $stream
    if ($newTcpText.Length -gt 0) {
      $tcpText += $newTcpText
      $trimmedTcp = $tcpText.Trim()
      if ($trimmedTcp.StartsWith("ERR")) {
        throw "Board returned TCP error before serial binary frame: '$trimmedTcp'. Please flash the latest Appli image with IRCAPIMG/IRCAPTEMP support."
      }
    }

    $buffer = $rx.ToArray()
    $beginIndex = Find-BytePattern -Buffer $buffer -Pattern $beginPattern
    if ($beginIndex -lt 0) {
      continue
    }

    $prefix = New-Object byte[] $beginIndex
    if ($beginIndex -gt 0) {
      [Array]::Copy($buffer, 0, $prefix, 0, $beginIndex)
    }
    $prefixText = [Text.Encoding]::ASCII.GetString($prefix)
    $headerMatch = [regex]::Match($prefixText, $headerRegex)
    if (-not $headerMatch.Success) {
      throw "Found BEGIN_TINY1C_BINARY but did not find a valid TINY1C_BIN header"
    }

    $removeCount = $beginIndex + $beginPattern.Length
    $rx.RemoveRange(0, $removeCount)
    break
  }

  $cmdHex = $headerMatch.Groups[1].Value.ToUpperInvariant()
  $width = [int]$headerMatch.Groups[2].Value
  $height = [int]$headerMatch.Groups[3].Value
  $byteCount = [int]$headerMatch.Groups[4].Value
  $source = $headerMatch.Groups[5].Value
  $expectedCrc = [Convert]::ToUInt32($headerMatch.Groups[6].Value, 16)
  $kind = Resolve-FrameKind -RequestedKind $FrameKind -CommandName $Command -InputPath "" -CmdHex $cmdHex

  Write-Host "Frame header: kind=$kind cmd=0x$cmdHex size=${width}x${height} bytes=$byteCount source=$source crc=0x$($headerMatch.Groups[6].Value.ToUpperInvariant())"

  while ($rx.Count -lt $byteCount) {
    if ($watch.ElapsedMilliseconds -gt $TimeoutMs) {
      throw "Timed out while reading frame payload: $($rx.Count)/$byteCount bytes"
    }
    [void](Read-SerialIntoBuffer -Serial $serial -Rx $rx -Scratch $scratch)
  }

  $frame = New-Object byte[] $byteCount
  $rx.CopyTo(0, $frame, 0, $byteCount)
  $rx.RemoveRange(0, $byteCount)

  $endWatch = [System.Diagnostics.Stopwatch]::StartNew()
  while ((Find-BytePattern -Buffer $rx.ToArray() -Pattern $endPattern) -lt 0) {
    if ($endWatch.ElapsedMilliseconds -gt 5000) {
      Write-Warning "Did not see END_TINY1C_BINARY before timeout; payload was already captured."
      break
    }
    [void](Read-SerialIntoBuffer -Serial $serial -Rx $rx -Scratch $scratch)
  }

  $actualCrc = Get-Crc32 -Data $frame
  if ($actualCrc -ne $expectedCrc) {
    throw ("CRC mismatch: expected=0x{0:X8} actual=0x{1:X8}" -f $expectedCrc, $actualCrc)
  }

  $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
  $saved = Save-Tiny1CFrameFiles -Frame $frame -Kind $kind -OutputDir $OutputDir -Stamp $stamp -Width $width -Height $height -ImageLane $ImageLane -Invert:$Invert

  Write-Host ("CRC OK: 0x{0:X8}" -f $actualCrc)
  Write-Host "Display mode: $($saved.Mode)"
  Write-Host "Pixel stats: nonzero=$($saved.NonZero) min=$($saved.Min) max=$($saved.Max)"
  Write-Host "Saved raw : $($saved.Raw)"
  Write-Host "Saved pgm : $($saved.Pgm16)"
  Write-Host "Saved pgm : $($saved.Pgm8)"
  Write-Host "Saved bmp : $($saved.Bmp)"
  if ($saved.U16Bmp -ne "") {
    Write-Host "Saved debug bmp: $($saved.U16Bmp)"
  }

  if ($null -ne $stream) {
    try {
      $response = New-Object byte[] 256
      if ($stream.DataAvailable) {
        $len = $stream.Read($response, 0, $response.Length)
        if ($len -gt 0) {
          $tcpText += [Text.Encoding]::ASCII.GetString($response, 0, $len)
        }
      }
      if ($tcpText.Trim().Length -gt 0) {
        Write-Host ("TCP response: " + $tcpText.Trim())
      }
    }
    catch {
      Write-Warning "TCP response was not read: $($_.Exception.Message)"
    }
  }
}
finally {
  if ($null -ne $tcp) {
    $tcp.Close()
  }
  if ($serial.IsOpen) {
    $serial.Close()
  }
}
