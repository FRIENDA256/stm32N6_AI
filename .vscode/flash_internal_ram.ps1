param(
    [Parameter(Mandatory = $true)][string]$OpenOcdPath,
    [Parameter(Mandatory = $true)][string]$SearchDir1,
    [Parameter(Mandatory = $true)][string]$SearchDir2,
    [Parameter(Mandatory = $true)][string]$InterfaceScript,
    [Parameter(Mandatory = $true)][string]$TargetScript,
    [Parameter(Mandatory = $true)][string]$FsblElf,
    [Parameter(Mandatory = $true)][string]$SecureElf,
    [Parameter(Mandatory = $true)][string]$NonSecureElf
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

    $dump = & arm-none-eabi-objdump -s -j .isr_vector $ElfPath
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to read .isr_vector from $ElfPath"
    }

    foreach ($line in $dump) {
        if ($line -match '^\s*[0-9a-fA-F]{8}\s+([0-9a-fA-F]{8})\s+([0-9a-fA-F]{8})') {
            return @{
                Vtor = [Convert]::ToUInt32($Matches[0].Trim().Split()[0], 16)
                Msp  = Convert-LeWordHexToUInt32 $Matches[1]
                Pc   = Convert-LeWordHexToUInt32 $Matches[2]
            }
        }
    }

    throw "Could not find vector table words in $ElfPath"
}

$secureVector = Get-VectorWords $SecureElf
$secureVtor = "0x{0:x8}" -f $secureVector.Vtor
$secureMsp = "0x{0:x8}" -f $secureVector.Msp
$securePc = "0x{0:x8}" -f $secureVector.Pc

Write-Host "Secure RAM entry: VTOR=$secureVtor MSP=$secureMsp PC=$securePc"

$openOcdArgs = @(
    "-s", $SearchDir1,
    "-s", $SearchDir2,
    "-f", $InterfaceScript,
    "-f", $TargetScript,
    "-c", "init; halt",
    "-c", "load_image $FsblElf",
    "-c", "verify_image $FsblElf",
    "-c", "load_image $SecureElf",
    "-c", "verify_image $SecureElf",
    "-c", "load_image $NonSecureElf",
    "-c", "verify_image $NonSecureElf",
    "-c", "mww 0xE000ED08 $secureVtor; reg msp $secureMsp; reg pc $securePc; resume; exit"
)

& $OpenOcdPath @openOcdArgs
exit $LASTEXITCODE
