param(
  [ValidateSet("ADGET", "ADNET", "IRGETIMG", "IRGETIMAGE", "IRGETTEMP", "IRNETIMG", "IRNETTEMP", "IRGETIMGBASE", "IRGETTEMPBASE", "CAMGET")]
  [string]$Command = "IRGETIMG",
  [string]$InputRaw = "",
  [ValidateSet("auto", "image", "temp")]
  [string]$FrameKind = "auto",
  [string]$HostAddress = "192.168.6.50",
  [int]$Port = 5000,
  [int]$TimeoutMs = 90000,
  [string]$OutputDir = ".\tools\net_captures",
  [ValidateSet("auto", "even", "odd")]
  [string]$ImageLane = "auto",
  [ValidateSet("le", "be")]
  [string]$Rgb565Endian = "le",
  [ValidateSet("row-auto", "le", "be")]
  [string]$TempEndian = "row-auto",
  [ValidateRange(1, 255)]
  [int]$ImageSpeckleThreshold = 48,
  [ValidateSet("none", "despeckle", "median3")]
  [string]$TempFilter = "despeckle",
  [int]$TempDespikeThreshold = 512,
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

function Read-TcpAvailable {
  param(
    [System.Net.Sockets.NetworkStream]$Stream,
    [System.Collections.Generic.List[byte]]$Rx,
    [byte[]]$Scratch
  )

  $total = 0
  while ($Stream.DataAvailable) {
    $count = $Stream.Read($Scratch, 0, $Scratch.Length)
    if ($count -le 0) {
      break
    }
    for ($i = 0; $i -lt $count; $i++) {
      $Rx.Add($Scratch[$i])
    }
    $total += $count
  }

  return $total
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

function Read-Le16 {
  param([byte[]]$Data, [int]$Offset)
  return [uint16](([int]$Data[$Offset]) -bor (([int]$Data[$Offset + 1]) -shl 8))
}

function Read-Le16S {
  param([byte[]]$Data, [int]$Offset)
  $value = [int](Read-Le16 -Data $Data -Offset $Offset)
  if ($value -ge 32768) {
    return ($value - 65536)
  }
  return $value
}

function Read-Le32 {
  param([byte[]]$Data, [int]$Offset)
  return [uint32](([uint32]$Data[$Offset]) -bor
                  (([uint32]$Data[$Offset + 1]) -shl 8) -bor
                  (([uint32]$Data[$Offset + 2]) -shl 16) -bor
                  (([uint32]$Data[$Offset + 3]) -shl 24))
}

function Read-Le64 {
  param([byte[]]$Data, [int]$Offset)
  $lo = [uint64](Read-Le32 -Data $Data -Offset $Offset)
  $hi = [uint64](Read-Le32 -Data $Data -Offset ($Offset + 4))
  return ($lo -bor ($hi -shl 32))
}

function Convert-HexField {
  param([string]$Text)

  if ($Text.StartsWith("0x", [System.StringComparison]::OrdinalIgnoreCase)) {
    return [Convert]::ToUInt32($Text.Substring(2), 16)
  }
  return [Convert]::ToUInt32($Text, 16)
}

function Convert-HeaderFields {
  param([string]$HeaderLine)

  $fields = @{}
  foreach ($match in [regex]::Matches($HeaderLine, '([A-Za-z0-9_]+)=([^ ]+)')) {
    $fields[$match.Groups[1].Value.ToUpperInvariant()] = $match.Groups[2].Value
  }
  return $fields
}

function Resolve-Tiny1CFrameKind {
  param(
    [string]$RequestedKind,
    [string]$CommandName,
    [string]$InputPath
  )

  if ($RequestedKind -ne "auto") {
    return $RequestedKind
  }
  if (($CommandName -match "TEMP") -or ($InputPath -match "(?i)temp")) {
    return "temp"
  }

  return "image"
}

function Convert-FrameToU16 {
  param(
    [byte[]]$Frame,
    [ValidateSet("le", "be")]
    [string]$Endian = "le"
  )

  $pixelCount = [int]($Frame.Length / 2)
  $pixels = New-Object 'System.UInt16[]' $pixelCount
  for ($i = 0; $i -lt $pixelCount; $i++) {
    $offset = $i * 2
    if ($Endian -eq "be") {
      $pixels[$i] = [uint16]((([uint16]$Frame[$offset]) -shl 8) -bor [uint16]$Frame[$offset + 1])
    }
    else {
      $pixels[$i] = [uint16](([uint16]$Frame[$offset]) -bor (([uint16]$Frame[$offset + 1]) -shl 8))
    }
  }

  return $pixels
}

function Get-U16Median {
  param([UInt16[]]$Values)

  if (($null -eq $Values) -or ($Values.Length -eq 0)) {
    return 0
  }

  $ints = New-Object 'System.Int32[]' $Values.Length
  for ($i = 0; $i -lt $Values.Length; $i++) {
    $ints[$i] = [int]$Values[$i]
  }
  [Array]::Sort($ints)

  return $ints[[int]($ints.Length / 2)]
}

function Convert-TempFrameToU16 {
  param(
    [byte[]]$Frame,
    [int]$Width,
    [int]$Height,
    [string]$EndianMode
  )

  $pixelCount = $Width * $Height
  $pixels = New-Object 'System.UInt16[]' $pixelCount
  $rowModes = New-Object 'System.String[]' $Height
  $targetRoomRaw = [int][Math]::Round((25.0 + 273.15) * 64.0)

  if ($EndianMode -eq "le") {
    return @{
      Pixels = (Convert-FrameToU16 -Frame $Frame -Endian "le")
      ModeSummary = "le"
    }
  }
  if ($EndianMode -eq "be") {
    return @{
      Pixels = (Convert-FrameToU16 -Frame $Frame -Endian "be")
      ModeSummary = "be"
    }
  }

  for ($y = 0; $y -lt $Height; $y++) {
    $rowLe = New-Object 'System.UInt16[]' $Width
    $rowBe = New-Object 'System.UInt16[]' $Width

    for ($x = 0; $x -lt $Width; $x++) {
      $offset = (($y * $Width) + $x) * 2
      $lo = [uint16]$Frame[$offset]
      $hi = [uint16]$Frame[$offset + 1]
      $rowLe[$x] = [uint16]($lo -bor ($hi -shl 8))
      $rowBe[$x] = [uint16](($lo -shl 8) -bor $hi)
    }

    $leMedian = Get-U16Median -Values $rowLe
    $beMedian = Get-U16Median -Values $rowBe
    $useBe = ([Math]::Abs($beMedian - $targetRoomRaw) -lt [Math]::Abs($leMedian - $targetRoomRaw))
    $rowModes[$y] = if ($useBe) { "B" } else { "L" }

    for ($x = 0; $x -lt $Width; $x++) {
      $pixels[($y * $Width) + $x] = if ($useBe) { $rowBe[$x] } else { $rowLe[$x] }
    }
  }

  $beCount = ($rowModes | Where-Object { $_ -eq "B" }).Count
  $leCount = $Height - $beCount

  return @{
    Pixels = $pixels
    ModeSummary = "row-auto le_rows=$leCount be_rows=$beCount"
  }
}

function Convert-U16ToGray8 {
  param(
    [UInt16[]]$Pixels,
    [switch]$PercentileStretch,
    [switch]$Invert
  )

  [uint32]$min = 65535
  [uint32]$max = 0
  [uint32]$nonzero = 0
  $nonzeroValues = [System.Collections.Generic.List[int]]::new()

  foreach ($pixel in $Pixels) {
    if ($pixel -ne 0) {
      $nonzero++
      $nonzeroValues.Add([int]$pixel)
      if ($pixel -lt $min) { $min = $pixel }
      if ($pixel -gt $max) { $max = $pixel }
    }
  }

  if ($nonzero -eq 0) {
    $min = 0
    $max = 65535
  }
  elseif ($PercentileStretch) {
    $ordered = $nonzeroValues.ToArray()
    [Array]::Sort($ordered)
    $lowIndex = [int][Math]::Floor(($ordered.Length - 1) * 0.01)
    $highIndex = [int][Math]::Ceiling(($ordered.Length - 1) * 0.99)
    $min = [uint32]$ordered[$lowIndex]
    $max = [uint32]$ordered[$highIndex]
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

function Get-IntMedian {
  param([int[]]$Values)

  if (($null -eq $Values) -or ($Values.Length -eq 0)) {
    return 0
  }

  [Array]::Sort($Values)
  return $Values[[int]($Values.Length / 2)]
}

function Repair-TempSpeckles {
  param(
    [UInt16[]]$Pixels,
    [int]$Width,
    [int]$Height,
    [string]$Mode,
    [int]$Threshold
  )

  if (($Mode -eq "none") -or ($Pixels.Length -ne ($Width * $Height))) {
    return @{
      Pixels = $Pixels
      Repaired = 0
      Summary = "none"
    }
  }

  $out = New-Object 'System.UInt16[]' $Pixels.Length
  [Array]::Copy($Pixels, $out, $Pixels.Length)
  $repaired = 0

  for ($y = 1; $y -lt ($Height - 1); $y++) {
    for ($x = 1; $x -lt ($Width - 1); $x++) {
      $idx = ($y * $Width) + $x
      $center = [int]$Pixels[$idx]
      if ($center -eq 0) {
        continue
      }

      $neighbors = [int[]]@(
        $Pixels[$idx - $Width - 1], $Pixels[$idx - $Width], $Pixels[$idx - $Width + 1],
        $Pixels[$idx - 1],                              $Pixels[$idx + 1],
        $Pixels[$idx + $Width - 1], $Pixels[$idx + $Width], $Pixels[$idx + $Width + 1]
      )
      $neighborMedian = Get-IntMedian -Values $neighbors
      if ($neighborMedian -eq 0) {
        continue
      }

      if ($Mode -eq "median3") {
        $all = [int[]]@(
          $Pixels[$idx - $Width - 1], $Pixels[$idx - $Width], $Pixels[$idx - $Width + 1],
          $Pixels[$idx - 1], $Pixels[$idx], $Pixels[$idx + 1],
          $Pixels[$idx + $Width - 1], $Pixels[$idx + $Width], $Pixels[$idx + $Width + 1]
        )
        $median = Get-IntMedian -Values $all
        if ($median -ne $center) {
          $out[$idx] = [uint16]$median
          $repaired++
        }
      }
      elseif ([Math]::Abs($center - $neighborMedian) -gt $Threshold) {
        $out[$idx] = [uint16]$neighborMedian
        $repaired++
      }
    }
  }

  return @{
    Pixels = $out
    Repaired = $repaired
    Summary = if ($Mode -eq "median3") { "median3 repaired=$repaired" } else { "despeckle threshold=$Threshold repaired=$repaired" }
  }
}

function Measure-TempSpatialJumps {
  param(
    [UInt16[]]$Pixels,
    [int]$Width,
    [int]$Height,
    [int]$Threshold
  )

  [uint32]$horizontal = 0
  [uint32]$vertical = 0
  [uint32]$comparisons = 0
  [uint32]$maxDelta = 0
  [uint32]$nonzero = 0
  [uint32]$minValue = 65535
  [uint32]$maxValue = 0

  if ($Pixels.Length -ne ($Width * $Height)) {
    return @{
      Horizontal = 0
      Vertical = 0
      Total = 0
      Comparisons = 0
      MaxDelta = 0
      NonZero = 0
      Min = 0
      Max = 0
    }
  }

  for ($y = 0; $y -lt $Height; $y++) {
    for ($x = 0; $x -lt $Width; $x++) {
      $idx = ($y * $Width) + $x
      $center = [int]$Pixels[$idx]
      if ($center -eq 0) {
        continue
      }

      $nonzero++
      if ($center -lt $minValue) { $minValue = [uint32]$center }
      if ($center -gt $maxValue) { $maxValue = [uint32]$center }

      if ($x -lt ($Width - 1)) {
        $right = [int]$Pixels[$idx + 1]
        if ($right -ne 0) {
          $delta = [uint32][Math]::Abs($center - $right)
          $comparisons++
          if ($delta -gt $Threshold) { $horizontal++ }
          if ($delta -gt $maxDelta) { $maxDelta = $delta }
        }
      }

      if ($y -lt ($Height - 1)) {
        $below = [int]$Pixels[$idx + $Width]
        if ($below -ne 0) {
          $delta = [uint32][Math]::Abs($center - $below)
          $comparisons++
          if ($delta -gt $Threshold) { $vertical++ }
          if ($delta -gt $maxDelta) { $maxDelta = $delta }
        }
      }
    }
  }

  if ($nonzero -eq 0) {
    $minValue = 0
  }

  return @{
    Horizontal = $horizontal
    Vertical = $vertical
    Total = $horizontal + $vertical
    Comparisons = $comparisons
    MaxDelta = $maxDelta
    NonZero = $nonzero
    Min = $minValue
    Max = $maxValue
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

function Measure-ImageSpatialNoise {
  param(
    [byte[]]$Gray,
    [int]$Width,
    [int]$Height,
    [int]$Threshold
  )

  [uint32]$horizontal = 0
  [uint32]$vertical = 0
  [uint32]$maxDelta = 0
  [uint32]$dark = 0
  [uint32]$bright = 0
  [uint32]$darkMax = 0
  [uint32]$brightMax = 0
  [uint32]$minValue = 255
  [uint32]$maxValue = 0
  $darkX = -1
  $darkY = -1
  $brightX = -1
  $brightY = -1

  if ($Gray.Length -ne ($Width * $Height)) {
    return @{
      Horizontal = 0; Vertical = 0; Total = 0; MaxDelta = 0
      Dark = 0; Bright = 0; DarkMax = 0; BrightMax = 0
      DarkX = -1; DarkY = -1; BrightX = -1; BrightY = -1
      Min = 0; Max = 0
    }
  }

  for ($y = 0; $y -lt $Height; $y++) {
    for ($x = 0; $x -lt $Width; $x++) {
      $idx = ($y * $Width) + $x
      $center = [int]$Gray[$idx]
      if ($center -lt $minValue) { $minValue = [uint32]$center }
      if ($center -gt $maxValue) { $maxValue = [uint32]$center }

      if ($x -lt ($Width - 1)) {
        $delta = [uint32][Math]::Abs($center - [int]$Gray[$idx + 1])
        if ($delta -gt $Threshold) { $horizontal++ }
        if ($delta -gt $maxDelta) { $maxDelta = $delta }
      }
      if ($y -lt ($Height - 1)) {
        $delta = [uint32][Math]::Abs($center - [int]$Gray[$idx + $Width])
        if ($delta -gt $Threshold) { $vertical++ }
        if ($delta -gt $maxDelta) { $maxDelta = $delta }
      }

      if (($x -eq 0) -or ($x -eq ($Width - 1)) -or ($y -eq 0) -or ($y -eq ($Height - 1))) {
        continue
      }

      $neighbors = [int[]]@(
        $Gray[$idx - $Width - 1], $Gray[$idx - $Width], $Gray[$idx - $Width + 1],
        $Gray[$idx - 1],                                  $Gray[$idx + 1],
        $Gray[$idx + $Width - 1], $Gray[$idx + $Width], $Gray[$idx + $Width + 1]
      )
      $median = Get-IntMedian -Values $neighbors
      $darkDelta = $median - $center
      $brightDelta = $center - $median

      if ($darkDelta -gt $Threshold) {
        $dark++
        if ($darkDelta -gt $darkMax) {
          $darkMax = [uint32]$darkDelta
          $darkX = $x
          $darkY = $y
        }
      }
      if ($brightDelta -gt $Threshold) {
        $bright++
        if ($brightDelta -gt $brightMax) {
          $brightMax = [uint32]$brightDelta
          $brightX = $x
          $brightY = $y
        }
      }
    }
  }

  return @{
    Horizontal = $horizontal
    Vertical = $vertical
    Total = $horizontal + $vertical
    MaxDelta = $maxDelta
    Dark = $dark
    Bright = $bright
    DarkMax = $darkMax
    BrightMax = $brightMax
    DarkX = $darkX
    DarkY = $darkY
    BrightX = $brightX
    BrightY = $brightY
    Min = $minValue
    Max = $maxValue
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

function Convert-Rgb565ToRgb24 {
  param(
    [byte[]]$Frame,
    [int]$Width,
    [int]$Height,
    [ValidateSet("le", "be")]
    [string]$Endian = "le"
  )

  $pixelCount = $Width * $Height
  $rgb = New-Object byte[] ($pixelCount * 3)

  for ($i = 0; $i -lt $pixelCount; $i++) {
    $offset = $i * 2
    if ($Endian -eq "be") {
      $value = (([int]$Frame[$offset]) -shl 8) -bor ([int]$Frame[$offset + 1])
    }
    else {
      $value = ([int]$Frame[$offset]) -bor (([int]$Frame[$offset + 1]) -shl 8)
    }

    $r5 = ($value -shr 11) -band 0x1F
    $g6 = ($value -shr 5) -band 0x3F
    $b5 = $value -band 0x1F
    $dst = $i * 3
    $rgb[$dst] = [byte][Math]::Round(($r5 * 255.0) / 31.0)
    $rgb[$dst + 1] = [byte][Math]::Round(($g6 * 255.0) / 63.0)
    $rgb[$dst + 2] = [byte][Math]::Round(($b5 * 255.0) / 31.0)
  }

  return $rgb
}

function Write-BmpRgb24 {
  param(
    [string]$Path,
    [byte[]]$Rgb,
    [int]$Width,
    [int]$Height
  )

  $srcRowBytes = $Width * 3
  $rowStride = (($srcRowBytes + 3) -band (-bnot 3))
  $imageSize = $rowStride * $Height
  $pixelOffset = 14 + 40
  $fileSize = $pixelOffset + $imageSize
  $padding = New-Object byte[] ($rowStride - $srcRowBytes)
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
    $writer.Write([uint16]24)
    $writer.Write([uint32]0)
    $writer.Write([uint32]$imageSize)
    $writer.Write([int32]2835)
    $writer.Write([int32]2835)
    $writer.Write([uint32]0)
    $writer.Write([uint32]0)

    for ($y = $Height - 1; $y -ge 0; $y--) {
      $rowOffset = $y * $srcRowBytes
      for ($x = 0; $x -lt $Width; $x++) {
        $src = $rowOffset + ($x * 3)
        $writer.Write($Rgb[$src + 2])
        $writer.Write($Rgb[$src + 1])
        $writer.Write($Rgb[$src])
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

function Save-Imx219Visual {
  param(
    [byte[]]$Frame,
    [hashtable]$Fields,
    [string]$BasePath,
    [ValidateSet("le", "be")]
    [string]$Rgb565Endian
  )

  $width = if ($Fields.ContainsKey("WIDTH")) { [int]$Fields["WIDTH"] } else { 640 }
  $height = if ($Fields.ContainsKey("HEIGHT")) { [int]$Fields["HEIGHT"] } else { 480 }
  $expectedBytes = $width * $height * 2

  if ($Frame.Length -ne $expectedBytes) {
    Write-Warning "IMX219 frame length does not match ${width}x${height}x2; skip BMP."
    return
  }

  $bmpPath = $BasePath + "_rgb565.bmp"
  $rgb = Convert-Rgb565ToRgb24 -Frame $Frame -Width $width -Height $height -Endian $Rgb565Endian
  Write-BmpRgb24 -Path $bmpPath -Rgb $rgb -Width $width -Height $height
  Write-Host "Saved bmp : $bmpPath"
  Write-Host "Display mode: IMX219 RGB565, endian=$Rgb565Endian"
}

function Save-Tiny1CVisual {
  param(
    [byte[]]$Frame,
    [hashtable]$Fields,
    [string]$BasePath,
    [string]$ImageLane,
    [int]$ImageSpeckleThreshold,
    [string]$TempEndian,
    [string]$TempFilter,
    [int]$TempDespikeThreshold,
    [switch]$Invert
  )

  $width = if ($Fields.ContainsKey("WIDTH")) { [int]$Fields["WIDTH"] } else { 256 }
  $height = if ($Fields.ContainsKey("HEIGHT")) { [int]$Fields["HEIGHT"] } else { 192 }
  $cmd = if ($Fields.ContainsKey("CMD")) { $Fields["CMD"].ToUpperInvariant() } else { "" }
  $kind = if ($cmd -eq "0xCC") { "temp" } else { "image" }
  $bmpPath = $BasePath + "_8bit.bmp"

  if ($Frame.Length -ne ($width * $height * 2)) {
    Write-Warning "Tiny1C frame length does not match ${width}x${height}x2; skip BMP."
    return
  }

  if ($kind -eq "image") {
    $gray = Convert-ImageFrameToGray8 -Frame $Frame -Width $width -Height $height -Lane $ImageLane
    $noiseInfo = Measure-ImageSpatialNoise -Gray $gray -Width $width -Height $height -Threshold $ImageSpeckleThreshold
    if ($Invert) {
      for ($i = 0; $i -lt $gray.Length; $i++) {
        $gray[$i] = [byte](255 - $gray[$i])
      }
    }
    Write-BmpGray8Palette -Path $bmpPath -Gray $gray -Width $width -Height $height
    Write-Host "Saved bmp : $bmpPath"
    Write-Host "Display mode: image y-only, lane=$ImageLane"
    Write-Host "Image raw stats: min=$($noiseInfo.Min) max=$($noiseInfo.Max)"
    Write-Host "Image spatial noise: threshold=$ImageSpeckleThreshold horizontal=$($noiseInfo.Horizontal) vertical=$($noiseInfo.Vertical) total=$($noiseInfo.Total) max_delta=$($noiseInfo.MaxDelta) dark=$($noiseInfo.Dark) bright=$($noiseInfo.Bright) dark_max=$($noiseInfo.DarkMax) dark_xy=$($noiseInfo.DarkX),$($noiseInfo.DarkY) bright_max=$($noiseInfo.BrightMax) bright_xy=$($noiseInfo.BrightX),$($noiseInfo.BrightY)"
    return
  }

  $tempInfo = Convert-TempFrameToU16 -Frame $Frame -Width $width -Height $height -EndianMode $TempEndian
  $pixels = $tempInfo.Pixels
  $jumpInfo = Measure-TempSpatialJumps -Pixels $pixels -Width $width -Height $height -Threshold $TempDespikeThreshold
  $rawGrayInfo = Convert-U16ToGray8 -Pixels $pixels -PercentileStretch -Invert:$Invert
  $unfilteredBmpPath = $BasePath + "_unfiltered_8bit.bmp"
  Write-BmpGray8Palette -Path $unfilteredBmpPath -Gray $rawGrayInfo.Bytes -Width $width -Height $height

  $filterInfo = Repair-TempSpeckles -Pixels $pixels -Width $width -Height $height -Mode $TempFilter -Threshold $TempDespikeThreshold
  $filteredPixels = $filterInfo.Pixels
  $grayInfo = Convert-U16ToGray8 -Pixels $filteredPixels -PercentileStretch -Invert:$Invert
  Write-BmpGray8Palette -Path $bmpPath -Gray $grayInfo.Bytes -Width $width -Height $height
  Write-Host "Saved bmp : $bmpPath"
  Write-Host "Saved raw-view bmp: $unfilteredBmpPath"
  Write-Host "Display mode: temp/u16 auto stretch, endian=$($tempInfo.ModeSummary)"
  Write-Host "Temp raw stats: nonzero=$($jumpInfo.NonZero) min=$($jumpInfo.Min) max=$($jumpInfo.Max)"
  Write-Host "Temp raw jumps: threshold=$TempDespikeThreshold horizontal=$($jumpInfo.Horizontal) vertical=$($jumpInfo.Vertical) total=$($jumpInfo.Total) max_delta=$($jumpInfo.MaxDelta) comparisons=$($jumpInfo.Comparisons)"
  Write-Host "Temp filter: $($filterInfo.Summary)"
  Write-Host "Pixel stats: nonzero=$($grayInfo.NonZero) min=$($grayInfo.Min) max=$($grayInfo.Max)"
}

function Save-Ad7606Csv {
  param(
    [byte[]]$Frame,
    [string]$CsvPath
  )

  if ($Frame.Length -lt 44) {
    return
  }

  $payloadOffset = 24
  $points = [int](Read-Le16 -Data $Frame -Offset $payloadOffset)
  $channels = [int]$Frame[$payloadOffset + 2]
  $bytesPerSample = [int]$Frame[$payloadOffset + 3]
  $samplesOffset = $payloadOffset + 20
  $expectedBytes = $samplesOffset + ($points * $channels * $bytesPerSample)

  if (($points -le 0) -or ($channels -le 0) -or ($channels -gt 16) -or ($bytesPerSample -ne 2) -or ($Frame.Length -lt $expectedBytes)) {
    return
  }

  $writer = [System.IO.StreamWriter]::new($CsvPath, $false, [Text.Encoding]::ASCII)
  try {
    $header = "idx"
    for ($ch = 0; $ch -lt $channels; $ch++) {
      $header += ",ch$($ch + 1)"
    }
    $writer.WriteLine($header)

    for ($row = 0; $row -lt $points; $row++) {
      $line = [string]$row
      for ($ch = 0; $ch -lt $channels; $ch++) {
        $offset = $samplesOffset + ((($row * $channels) + $ch) * 2)
        $line += "," + (Read-Le16S -Data $Frame -Offset $offset)
      }
      $writer.WriteLine($line)
    }
  }
  finally {
    $writer.Dispose()
  }
}

function Show-Ad7606Summary {
  param(
    [byte[]]$Frame,
    [string]$CsvPath
  )

  if ($Frame.Length -lt 28) {
    Write-Warning "AD7606 frame too short for header."
    return
  }

  $magic = Read-Le16 -Data $Frame -Offset 0
  $version = [int]$Frame[2]
  $type = [int]$Frame[3]
  $total = Read-Le16 -Data $Frame -Offset 4
  $payload = Read-Le16 -Data $Frame -Offset 6
  $seq = Read-Le32 -Data $Frame -Offset 8
  $ts = Read-Le32 -Data $Frame -Offset 12
  $sample = Read-Le32 -Data $Frame -Offset 16
  $frameCrc = Read-Le32 -Data $Frame -Offset ($Frame.Length - 4)

  Write-Host ("AD7606 frame: magic=0x{0:X4} ver={1} type=0x{2:X2} total={3} payload={4} seq={5} ts={6} sample={7} frame_crc=0x{8:X8}" -f $magic, $version, $type, $total, $payload, $seq, $ts, $sample, $frameCrc)

  if (($type -ne 1) -or ($Frame.Length -lt 44)) {
    return
  }

  $payloadOffset = 24
  $points = [int](Read-Le16 -Data $Frame -Offset $payloadOffset)
  $channels = [int]$Frame[$payloadOffset + 2]
  $bytesPerSample = [int]$Frame[$payloadOffset + 3]
  $blockStart = Read-Le64 -Data $Frame -Offset ($payloadOffset + 4)
  $blockEnd = Read-Le64 -Data $Frame -Offset ($payloadOffset + 12)
  Write-Host ("AD7606 raw: points={0} channels={1} bytes_per_sample={2} block=0x{3:X16}..0x{4:X16}" -f $points, $channels, $bytesPerSample, $blockStart, $blockEnd)

  if (($channels -gt 3) -and ($bytesPerSample -eq 2) -and ($points -gt 0)) {
    $samplesOffset = $payloadOffset + 20
    [int]$min = 32767
    [int]$max = -32768
    [int64]$sum = 0
    $limit = [Math]::Min($points, [int](($Frame.Length - $samplesOffset) / ($channels * 2)))

    for ($row = 0; $row -lt $limit; $row++) {
      $offset = $samplesOffset + ((($row * $channels) + 3) * 2)
      $value = Read-Le16S -Data $Frame -Offset $offset
      if ($value -lt $min) { $min = $value }
      if ($value -gt $max) { $max = $value }
      $sum += $value
    }

    if ($limit -gt 0) {
      Write-Host ("AD7606 CH4 stats: count={0} min={1} max={2} avg={3}" -f $limit, $min, $max, [int]($sum / $limit))
    }
  }

  Save-Ad7606Csv -Frame $Frame -CsvPath $CsvPath
  if (Test-Path -LiteralPath $CsvPath) {
    Write-Host "Saved csv : $CsvPath"
  }
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

if (-not [string]::IsNullOrWhiteSpace($InputRaw)) {
  $rawPath = (Resolve-Path -LiteralPath $InputRaw).Path
  $payload = [System.IO.File]::ReadAllBytes($rawPath)
  if ($payload.Length -ne (256 * 192 * 2)) {
    throw "InputRaw must be exactly 98304 bytes for Tiny1C frames, got $($payload.Length): $rawPath"
  }

  $kind = Resolve-Tiny1CFrameKind -RequestedKind $FrameKind -CommandName $Command -InputPath $rawPath
  $fields = @{
    SOURCE = "TINY1C"
    KIND = $kind
    CMD = if ($kind -eq "temp") { "0xCC" } else { "0xAA" }
    WIDTH = "256"
    HEIGHT = "192"
    BYTES = [string]$payload.Length
  }
  $stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
  $basePath = Join-Path $OutputDir ("tiny1c_${kind}_${stamp}_offline")
  Save-Tiny1CVisual -Frame $payload -Fields $fields -BasePath $basePath -ImageLane $ImageLane -ImageSpeckleThreshold $ImageSpeckleThreshold -TempEndian $TempEndian -TempFilter $TempFilter -TempDespikeThreshold $TempDespikeThreshold -Invert:$Invert
  Write-Host "Offline raw : $rawPath"
  return
}

$beginPattern = [Text.Encoding]::ASCII.GetBytes("BEGIN_STM32N6_BINARY`r`n")
$endPattern = [Text.Encoding]::ASCII.GetBytes("END_STM32N6_BINARY")
$tcp = [System.Net.Sockets.TcpClient]::new()
$stream = $null

try {
  Write-Host "Connecting $HostAddress`:$Port..."
  $tcp.NoDelay = $true
  $tcp.Connect($HostAddress, $Port)
  $stream = $tcp.GetStream()

  Write-Host "Sending TCP command $Command..."
  $cmdBytes = [Text.Encoding]::ASCII.GetBytes($Command + "`r`n")
  [void]$stream.Write($cmdBytes, 0, $cmdBytes.Length)

  $rx = [System.Collections.Generic.List[byte]]::new()
  $scratch = New-Object byte[] 4096
  $watch = [System.Diagnostics.Stopwatch]::StartNew()
  $headerLine = ""

  while ($true) {
    if ($watch.ElapsedMilliseconds -gt $TimeoutMs) {
      $tail = Get-AsciiTail -Buffer $rx.ToArray()
      throw "Timed out waiting for BEGIN_STM32N6_BINARY. Tail='$tail'"
    }

    [void](Read-TcpAvailable -Stream $stream -Rx $rx -Scratch $scratch)
    $buffer = $rx.ToArray()

    if ($buffer.Length -gt 0) {
      $tailText = Get-AsciiTail -Buffer $buffer
      if (($tailText -match '(^|\r|\n)ERR ') -and ($tailText -notmatch 'BEGIN_STM32N6_BINARY')) {
        throw "Board returned error: $tailText"
      }
    }

    $beginIndex = Find-BytePattern -Buffer $buffer -Pattern $beginPattern
    if ($beginIndex -ge 0) {
      $prefix = New-Object byte[] $beginIndex
      if ($beginIndex -gt 0) {
        [Array]::Copy($buffer, 0, $prefix, 0, $beginIndex)
      }

      $prefixText = [Text.Encoding]::ASCII.GetString($prefix)
      $headerLine = (($prefixText -split "\r?\n") | Where-Object { $_ -match '^STM32N6_BIN V1 ' } | Select-Object -Last 1)
      if ([string]::IsNullOrWhiteSpace($headerLine)) {
        throw "Found BEGIN_STM32N6_BINARY but no STM32N6_BIN header."
      }

      $removeCount = $beginIndex + $beginPattern.Length
      $rx.RemoveRange(0, $removeCount)
      break
    }

    Start-Sleep -Milliseconds 5
  }

  $fields = Convert-HeaderFields -HeaderLine $headerLine
  if ((-not $fields.ContainsKey("BYTES")) -or (-not $fields.ContainsKey("CRC32")) -or (-not $fields.ContainsKey("SOURCE"))) {
    throw "Header misses SOURCE/BYTES/CRC32: $headerLine"
  }

  $byteCount = [int]$fields["BYTES"]
  $expectedCrc = Convert-HexField -Text $fields["CRC32"]
  Write-Host "Frame header: $headerLine"

  while ($rx.Count -lt $byteCount) {
    if ($watch.ElapsedMilliseconds -gt $TimeoutMs) {
      throw "Timed out while reading payload: $($rx.Count)/$byteCount bytes"
    }
    [void](Read-TcpAvailable -Stream $stream -Rx $rx -Scratch $scratch)
    Start-Sleep -Milliseconds 1
  }

  $payload = New-Object byte[] $byteCount
  $rx.CopyTo(0, $payload, 0, $byteCount)
  $rx.RemoveRange(0, $byteCount)

  $endWatch = [System.Diagnostics.Stopwatch]::StartNew()
  while ((Find-BytePattern -Buffer $rx.ToArray() -Pattern $endPattern) -lt 0) {
    if ($endWatch.ElapsedMilliseconds -gt 3000) {
      Write-Warning "Did not see END_STM32N6_BINARY before timeout; payload was already captured."
      break
    }
    [void](Read-TcpAvailable -Stream $stream -Rx $rx -Scratch $scratch)
    Start-Sleep -Milliseconds 5
  }

  $actualCrc = Get-Crc32 -Data $payload
  if ($actualCrc -ne $expectedCrc) {
    throw ("CRC mismatch: expected=0x{0:X8} actual=0x{1:X8}" -f $expectedCrc, $actualCrc)
  }

  $source = $fields["SOURCE"].ToLowerInvariant()
  $stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
  $kind = $source
  if ($source -eq "tiny1c") {
    $kind = if (($fields.ContainsKey("CMD")) -and ($fields["CMD"].ToUpperInvariant() -eq "0xCC")) { "tiny1c_temp" } else { "tiny1c_image" }
  }
  elseif ($source -eq "ad7606") {
    $kind = "ad7606"
  }
  elseif ($source -eq "imx219") {
    $kind = "imx219_rgb565"
  }

  $basePath = Join-Path $OutputDir ("${kind}_${stamp}")
  $rawPath = $basePath + ".raw"
  $headerPath = $basePath + ".txt"
  [System.IO.File]::WriteAllBytes($rawPath, $payload)
  [System.IO.File]::WriteAllText($headerPath, $headerLine + "`r`n", [Text.Encoding]::ASCII)

  Write-Host ("CRC OK: 0x{0:X8}" -f $actualCrc)
  Write-Host "Saved raw : $rawPath"
  Write-Host "Saved hdr : $headerPath"

  if ($source -eq "tiny1c") {
    Save-Tiny1CVisual -Frame $payload -Fields $fields -BasePath $basePath -ImageLane $ImageLane -ImageSpeckleThreshold $ImageSpeckleThreshold -TempEndian $TempEndian -TempFilter $TempFilter -TempDespikeThreshold $TempDespikeThreshold -Invert:$Invert
  }
  elseif ($source -eq "ad7606") {
    Show-Ad7606Summary -Frame $payload -CsvPath ($basePath + "_samples.csv")
  }
  elseif ($source -eq "imx219") {
    Save-Imx219Visual -Frame $payload -Fields $fields -BasePath $basePath -Rgb565Endian $Rgb565Endian
  }
}
finally {
  if ($null -ne $tcp) {
    $tcp.Close()
  }
}
