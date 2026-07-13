# fsbl_appli_all Ethernet Baseline

This document records the Ethernet configuration that is known to work in
`fsbl_appli_all`. It exists because several of these settings are easy to lose
after CubeMX regeneration or when copying files from another variant.

Known-good smoke test:

```powershell
$tcp = [System.Net.Sockets.TcpClient]::new()
$tcp.Connect("192.168.6.50", 5000)
$stream = $tcp.GetStream()
$cmd = [Text.Encoding]::ASCII.GetBytes("PING`r`n")
[void]$stream.Write($cmd, 0, $cmd.Length)
$buf = New-Object byte[] 128
$len = $stream.Read($buf, 0, $buf.Length)
[Text.Encoding]::ASCII.GetString($buf, 0, $len)
$tcp.Close()
```

Expected response:

```text
PONG
```

## Critical Contract

The following items must stay true.

| Area | Required setting | Why it matters |
| --- | --- | --- |
| RTL8211F PHY | `rtl8211_enable_rxc_output()` is present and called during PHY init | Without RXC output, the PHY can report link up while the STM32 MAC receives no ARP, ICMP, or TCP SYN frames. |
| RTL8211F PHY | `LIMIT_RTL8211F_TO_100M_FULL=1` | Keeps negotiation on the validated 100M full-duplex path. |
| RTL8211F PHY | `ENABLE_RTL8211F_TXDELAY=1` and `ENABLE_RTL8211F_RXDELAY=1` | Preserves RGMII timing margin. |
| RTL8211F PHY | `DISABLE_RTL8211F_EEE=1` | Avoids low-power Ethernet behavior during bring-up. |
| NetX Duo | `NX_IP_PERIODIC_RATE` is fixed to `1000` | Matches ThreadX tick rate; wrong values make NetX timing and LED cadence misleading. |
| NetX Duo | `ETH_PHY_1000MBITS_SUPPORTED` is defined | Keeps the ST Ethernet driver and RTL8211F link-state handling aligned. |
| ETH DMA | `.RxDecripSection` at `0x34100000` and `.TxDecripSection` at `0x341000C0` | Keeps ETH descriptors in the expected fixed, aligned RAM locations. |
| FSBL boot | `MX_EXTMEM_Init()` and `BOOT_Application()` are called | Ensures FSBL actually loads and jumps to Appli. |
| RIF | ETH1 master/slave attributes remain secure accessible | Prevents secure-access faults when ETH is initialized by Appli. |

## Files To Watch

These files are the main places that CubeMX or manual merges can accidentally
damage:

```text
Middlewares/ST/netxduo/common/drivers/ethernet/rtl8211/nx_stm32_phy_driver.c
Drivers/BSP/Components/rtl8211/rtl8211.c
Appli/NetXDuo/App/nx_user.h
Appli/NetXDuo/Target/nx_stm32_eth_config.h
Makefile/Appli/Makefile
Makefile/Appli/STM32N657XX_LRUN.ld
Appli/Core/Src/eth.c
Appli/Core/Src/main.c
Appli/Core/Src/stm32n6xx_it.c
FSBL/Core/Src/main.c
Makefile/FSBL/Makefile
```

## Automatic Check

Run the guard script from the `fsbl_appli_all` project root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\check_ethernet_baseline.ps1
```

For a stricter post-build check, require generated artifacts too:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\check_ethernet_baseline.ps1 -RequireBuildArtifacts
```

Recommended workflow after CubeMX regeneration:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\check_ethernet_baseline.ps1
make -C Makefile\Appli -j8
make -C Makefile\FSBL -j8
```

If the script fails, do not flash the image yet. Fix the missing baseline item
first, then rebuild and re-sign.

## Board Test Checklist

Expected UART startup lines:

```text
FSBL->Appli all Ethernet start
ETH HCLK Hz: ...
ETH kernel Hz: ...
NetX init OK
IP: 192.168.6.50/24
NetX link: up
UDP echo: 192.168.6.50:5005
TCP cmd: 192.168.6.50:5000
```

PC-side quick test:

```powershell
arp -d *
ping 192.168.6.50
```

Then run the TCP `PING` test shown at the top of this document.

## Failure Hints

If UART prints `NetX link: up` but `ping` and TCP timeout, check these first:

1. `rtl8211_enable_rxc_output()` exists and is called.
2. `RTL8211_PHYCR2_RXC_ENABLE` is written on page `0x0A43`.
3. `.RxDecripSection` and `.TxDecripSection` are present in the linker script.
4. `DMARxDscrTab` is at `0x34100000` and `DMATxDscrTab` is at `0x341000C0` in the map file.
5. `NX_IP_PERIODIC_RATE` is `1000`.

If FSBL boots but there is no Appli UART output, check:

1. `FSBL/Core/Src/main.c` calls `MX_EXTMEM_Init()`.
2. `FSBL/Core/Src/main.c` calls `BOOT_Application()`.
3. FSBL Makefile includes the STM32 ExtMem Manager and boot LRUN sources.
