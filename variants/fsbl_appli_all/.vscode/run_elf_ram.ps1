param(
    [Parameter(Mandatory = $true)][string]$OpenOcdPath,
    [Parameter(Mandatory = $true)][string]$ObjdumpPath,
    [Parameter(Mandatory = $true)][string]$SearchDir1,
    [Parameter(Mandatory = $true)][string]$SearchDir2,
    [Parameter(Mandatory = $true)][string]$InterfaceScript,
    [Parameter(Mandatory = $true)][string]$TargetScript,
    [Parameter(Mandatory = $true)][string]$Elf
)

$ErrorActionPreference = "Stop"

function Convert-LeWordHexToUInt32 {
    param([Parameter(Mandatory = $true)][string]$WordHex)

    if ($WordHex.Length -ne 8) {
        throw "Invalid 32-bit word: $WordHex"
    }

    $beHex = $WordHex.Substring(6, 2) + $WordHex.Substring(4, 2) + $WordHex.Substring(2, 2) + $WordHex.Substring(0, 2)
    return [Convert]::ToUInt32($beHex, 16)
}

function Get-VectorWords {
    param([Parameter(Mandatory = $true)][string]$ElfPath)

    $dump = & $ObjdumpPath -s -j .isr_vector $ElfPath
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to read .isr_vector from $ElfPath"
    }

    foreach ($line in $dump) {
        if ($line -match '^\s*([0-9a-fA-F]{8})\s+([0-9a-fA-F]{8})\s+([0-9a-fA-F]{8})') {
            return @{
                Vtor = [Convert]::ToUInt32($Matches[1], 16)
                Msp  = Convert-LeWordHexToUInt32 $Matches[2]
                Pc   = Convert-LeWordHexToUInt32 $Matches[3]
            }
        }
    }

    throw "Could not find vector table words in $ElfPath"
}

$vector = Get-VectorWords $Elf
$vtor = "0x{0:x8}" -f $vector.Vtor
$msp = "0x{0:x8}" -f $vector.Msp
$pc = "0x{0:x8}" -f $vector.Pc

Write-Host "RAM entry: VTOR=$vtor MSP=$msp PC=$pc"

$openOcdArgs = @(
    "-s", $SearchDir1,
    "-s", $SearchDir2,
    "-f", $InterfaceScript,
    "-f", $TargetScript,
    "-c", "init; halt",
    "-c", "load_image $Elf",
    "-c", "verify_image $Elf",
    "-c", "mww 0xE000ED08 $vtor; reg msp $msp; reg pc $pc; resume; exit"
)

& $OpenOcdPath @openOcdArgs
exit $LASTEXITCODE
