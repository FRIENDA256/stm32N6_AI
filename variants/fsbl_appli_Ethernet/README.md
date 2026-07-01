# FSBL Appli Ethernet Bring-up

This variant started from the clean FSBL -> Appli LED/USART baseline and is now
used for early Ethernet hardware bring-up on an STM32N657 design with an
RTL8211F RGMII PHY.

The low-level Ethernet TX path has been proven before with raw broadcast
frames. The current image is temporarily switched back to a raw TX diagnostic
mode with `APP_ETH_RAW_TX_TEST=1` so the STM32 -> PHY -> magnetics/RJ45 -> PC
direction can be rechecked independently of NetX Duo RX/ARP handling.

After this TX-only check, set `APP_ETH_RAW_TX_TEST` back to `0` or remove the
Makefile define to return to the ThreadX + NetX Duo static IPv4 ping test.

## Current Boot Chain

```text
FSBL -> Appli
```

Runtime behavior in the current raw TX diagnostic image:

- FSBL initializes the external Flash path through XSPI2/EXTMEM.
- FSBL jumps to the secure Appli image.
- Appli initializes GPIO, USART3, ETH1, and RIF.
- `Ethernet_BringupTests_BeforeNetX()` enters an infinite raw Ethernet TX loop.
- ThreadX and NetX Duo are intentionally not started in this image.
- The board transmits 60-byte broadcast Ethernet frames with EtherType
  `0x88B5` and source MAC `02:00:00:00:00:01`.

Expected serial output now begins with:

```text
FSBL->Appli NetX Ethernet start
RAW TX test: eth.type=0x88B5 src=02:00:00:00:00:01
```

The previous NetX Duo path remains in the project, but is bypassed while
`APP_ETH_RAW_TX_TEST=1`.

## Hardware Context

The Ethernet design under test uses:

- STM32N657 ETH1 in RGMII mode.
- RTL8211F-CG PHY at MDIO address `0x01`.
- PHY local 25 MHz crystal. `PF5 = ETH1_CLK` must not be muxed or driven on this
  board. When PF5 was left in ETH alternate-function mode, the `ETH_CLK` net
  coupled a 100 MHz clock through the optional R118 path and corrupted the
  RTL8211F 25 MHz crystal/EXT_CLK node.
- RGMII TX/RX bus between STM32N657 and RTL8211F.
- External 4-channel gigabit Ethernet magnetics between RTL8211F MDI pins and a
  plain RJ45 connector.

The observed PHY ID is:

```text
PHY ID1 = 0x001C
PHY ID2 = 0xC916
```

That matches the expected RTL8211F family.

## CubeMX Configuration

Current CubeMX/C-generated Ethernet setup:

- `ETH1` enabled in `RGMII` mode.
- Application runtime context enabled for ETH1.
- ETH1 interrupt enabled in NVIC.
- ETH GPIOs are manually aligned with the official STM32N6570-DK NetX example:
  - GPIOD MDIO/MDC/INT pins: no pull, very high speed.
  - GPIOF/ GPIOG RGMII data/control pins: pull-up, very high speed.
  - PF0 RGMII GTX clock: pull-up, medium speed.
- ETH1 interrupt priority is set to `7`, matching the official DK example.
- Descriptor count kept small for bring-up:
  - TX descriptors: 4
  - RX descriptors: 4
  - RX buffer length: 1536
- Descriptor base addresses kept in CPUAXI SRAM:
  - RX descriptor address: `0x34100000`
  - TX descriptor address: `0x341000C0`

Important pin mapping currently generated:

```text
PD1  -> ETH1_MDC
PD12 -> ETH1_MDIO
PD3  -> ETH1_PHY_INTN
PF0  -> ETH1_RGMII_GTX_CLK
PF2  -> ETH1_RGMII_CLK125
PF7  -> ETH1_RGMII_RX_CLK
PF8  -> ETH1_RGMII_RXD2
PF9  -> ETH1_RGMII_RXD3
PF10 -> ETH1_RGMII_RX_CTL
PF11 -> ETH1_RGMII_TX_CTL
PF12 -> ETH1_RGMII_TXD0
PF13 -> ETH1_RGMII_TXD1
PF14 -> ETH1_RGMII_RXD0
PF15 -> ETH1_RGMII_RXD1
PG3  -> ETH1_RGMII_TXD2
PG4  -> ETH1_RGMII_TXD3
```

## Code Changes Made

### Code Organization

`Appli/Core/Src/main.c` is now kept as the boot and initialization sequence
only. Bring-up utilities were split into small modules:

- `Appli/Core/Src/app_console.c` / `Appli/Core/Inc/app_console.h`
  - bounded polled USART prints
  - boot-stage marker prints
  - hex formatting helpers
- `Appli/Core/Src/app_timebase.c` / `Appli/Core/Inc/app_timebase.h`
  - pre-ThreadX SysTick stop helper
  - `HAL_GetTick()` override that switches to the ThreadX tick after the RTOS
    timer starts
- `Appli/Core/Src/eth_diagnostics.c` / `Appli/Core/Inc/eth_diagnostics.h`
  - shared Ethernet clock diagnostics
- `Appli/Core/Src/eth_bringup_tests.c` / `Appli/Core/Inc/eth_bringup_tests.h`
  - reserved low-level Ethernet test hooks
  - current NetX build leaves old raw PHY/TX tests inactive

The Appli Makefile has been updated to compile these new source files. If
CubeMX regenerates the Makefile, re-add those four `.c` files.

### ETH Clock

`Appli/Core/Src/eth.c` was adjusted to keep the system HCLK at 200 MHz while
feeding ETH1 from a 100 MHz kernel clock, matching the official STM32N6570-DK
Ethernet examples:

```text
ETH1 source: IC12
IC12 source: PLL1
IC12 divider: 12
ETH kernel clock: 100 MHz
```

Typical diagnostic output:

```text
ETH HCLK Hz:   0x0BEBC200  // 200 MHz
ETH kernel Hz: 0x05F5E100  // 100 MHz
```

Important board-specific fix: `PF5 = ETH1_CLK` is intentionally not initialized
as an ETH alternate-function pin in `HAL_ETH_MspInit()`. The RTL8211F uses its
own 25 MHz crystal, so PF5 must remain disconnected/high-Z from that clock node.

### Official STM32N6570-DK Comparison

The closest official reference checked is:

```text
STM32Cube_FW_N6_V1.3.0/Projects/STM32N6570-DK/Applications/NetXDuo/Nx_WebServer
```

Relevant findings from the official project:

- It uses `ETH1` in RGMII mode with the same RTL8211 PHY component driver.
- Its ETH kernel clock is 100 MHz. The DK reaches that through `HCLK`; this
  Appli keeps HCLK at 200 MHz and uses IC12/PLL1 divided to 100 MHz.
- Its descriptor addresses are higher in CPUAXI SRAM
  (`0x341D4000` and `0x341D40C0`). This project currently keeps descriptors at
  `0x34100000` and `0x341000C0`, which is still valid and already worked for
  raw TX.
- Its NetX memory pools are placed into dedicated linker sections. This project
  still leaves the generated NetX pool in normal static RAM while DCache is
  disabled. If DCache/MPU is enabled later, copy the official dedicated-section
  approach before doing performance testing.
- Its NetX application enables DHCP, TCP/UDP, and Web Server threads. This
  project intentionally keeps a smaller static-IP ping test. For that target,
  the required packet pool, IP instance, ARP, and ICMP setup is already present;
  if ping still fails, check the NetX driver receive path and the ARP/ICMP
  counters printed by the status thread.
- Its NetX application also creates a dedicated link-management thread. This
  project now mirrors that pattern with a smaller static-IP version:
  `NetXDuo_LinkThreadEntry()` periodically checks `NX_IP_LINK_ENABLED` and sends
  `NX_LINK_ENABLE` / `NX_LINK_DISABLE` direct commands to the ST Ethernet driver
  when the cable state changes. Its initial state intentionally forces one
  `NX_LINK_ENABLE` attempt on the first observed link-up event.
- Its RIF setup grants ETH1 both master and slave access. This project now adds
  the ETH1 slave secure attribute too, using the Appli's secure/non-privileged
  access model.
- Its ETH GPIO setup uses pull-ups on the RGMII data/control pins and PF0 GTX
  clock. This project now follows that, while still excluding PF5 because of the
  custom board's removed R118/PF5 clock path.

### PHY Diagnostics

The following MDIO/RTL8211F diagnostics were used during bare Ethernet bring-up
and are now treated as historical test coverage rather than normal NetX startup
code:

- Read PHY ID, BMCR, BMSR.
- Perform an RTL8211F software reset.
- Read autonegotiation registers:
  - `ANAR`
  - `ANLPAR`
  - `ANER`
  - `1000BASE-T CTRL/STAT`
  - `EXT STATUS`
- Read RTL8211F RGMII delay strap state.
- Read RTL8211F LED configuration.
- Read RTL8211F page `0x0A43` status registers:
  - `PHYCR1`
  - `PHYCR2`
  - `PHYSR1`
  - `INSR`

Current strap observation:

```text
TXDLY strap: 0x0009
RXDLY strap: 0x0019
```

This means the board strap is currently enabling RX delay but not TX delay.

### Link Bring-up Fallback

During early hardware bring-up, autonegotiation was unstable on the original
MDI path and the firmware temporarily fell back to a constrained test mode:

```text
10 Mbps full duplex
manual MDI mode selection test
```

That forced 10M mode was only for minimum hardware verification. It is not the
final production configuration and is not part of the current NetX startup path.

### NetX Duo Static IPv4 Test

CubeMX has generated ThreadX and NetX Duo support for the Application context.
The current application code uses the generated ST Ethernet NetX driver and the
RTL8211 PHY component driver.

One local driver fix is applied under
`Middlewares/ST/netxduo/common/drivers/ethernet/nx_stm32_eth_driver.c`:
`FilterConfig.BroadcastFilter` is kept `DISABLE` and the MAC filter is applied
during hardware initialization. In the STM32 HAL, enabling this field sets the
`ETH_MACPFR_DBF` bit, which disables broadcast packet reception. That blocks
ARP requests such as `Who has 192.168.1.50?`, so it must remain disabled for
the static IPv4 ping test.

Current first-stack test settings:

```text
MAC address: 02:00:00:00:00:01
IPv4:        192.168.1.50
Netmask:     255.255.255.0
Gateway:     none
DHCP:        disabled
IPv6:        disabled
ICMP:        enabled
```

PC-side test setup:

```text
PC Ethernet IPv4: 192.168.1.10
Netmask:          255.255.255.0
Gateway:          blank
```

Then test:

```sh
ping 192.168.1.50
```

Useful Wireshark display filters:

```text
arp || icmp
eth.addr == 02:00:00:00:00:01
arp || icmp || eth.addr == 02:00:00:00:00:01
```

Expected serial output from the NetX test:

```text
NetX Duo init start
NetX Duo init done
NetX Duo static IPv4: 192.168.1.50/24
PC test: set 192.168.1.x/24, then ping 192.168.1.50
NetX link: up
```

ThreadX takes over SysTick, so `HAL_GetTick()` is overridden in
`Appli/Core/Src/app_timebase.c` to use the ThreadX tick after the RTOS timer starts.
This keeps the ST Ethernet/RTL8211 driver timeouts working without adding a
separate TIM6 HAL time base.

### Raw Ethernet Transmit Test

The current build enables this test with:

```make
-DAPP_ETH_RAW_TX_TEST=1
```

It is implemented in `Appli/Core/Src/eth_bringup_tests.c` and runs before
`MX_ThreadX_Init()`. This deliberately bypasses NetX Duo and avoids depending
on the ThreadX SysTick handler during the low-level hardware test.

The raw test sends a 60-byte Ethernet broadcast frame without a TCP/IP stack:

```text
Destination MAC: ff:ff:ff:ff:ff:ff
Source MAC:      02:00:00:00:00:01
EtherType:       0x88B5
Payload prefix:  STM32N6 RAW TX SEQ=
```

It also limits the RTL8211F advertisement to 100M full duplex, enables RTL8211F
RGMII RXC output, enables RGMII TX/RX internal delays, configures the STM32 ETH
MAC for 100M full duplex, starts the ETH HAL, and repeatedly calls
`HAL_ETH_Transmit_IT()`.

Useful Wireshark display filter:

```text
eth.type == 0x88b5 || eth.src == 02:00:00:00:00:01
```

If Wireshark captures these frames, the board TX direction is proven through:

```text
CPU buffer -> ETH DMA -> STM32 MAC -> RGMII TX -> RTL8211F -> MDI/magnetics/RJ45 -> PC
```

This does not prove the RX direction. The previous NetX issue, where PC ARP
requests were visible in Wireshark but `ip_rx`, `arp_req`, `irq`, and `mac_rx`
stayed zero, still points at the PHY-to-STM32 RGMII RX side.

### DMA And Cache Diagnostics

These were used during the raw Ethernet bring-up phase and are no longer part
of the normal NetX startup path:

- CPU DCache is checked and kept disabled for the ETH DMA test path.
- ETH descriptors are aligned and placed in CPUAXI SRAM.
- DMA descriptor skip length is configured to 32-bit.
- DMA/MTL/MAC debug registers are printed on transmit failure.
- MMC TX good packet counters are printed to confirm whether the MAC believes a
  frame was transmitted.

Observed good sign:

```text
ETH MMC TX good packets increments
ETH HAL raw TX seq low16 increments
```

## Test Results So Far

### Confirmed Working

- FSBL -> Appli boot path still works.
- USART3 diagnostics and heartbeat still work.
- MDIO/MDC communication works.
- RTL8211F PHY address and ID are correct.
- PHY 25 MHz crystal is present.
- ETH1 kernel clock has been corrected to 100 MHz.
- RGMII TX signals are present:
  - `PF0 / ETH1_RGMII_GTX_CLK`
  - `PF11 / ETH1_RGMII_TX_CTL`
  - `TXD0..TXD3`
- STM32 ETH MAC/DMA can submit raw frames.
- MMC TX good-packet counter increments.
- PHY-side D2 pair shows expected 10BASE-T-like transmit activity after raw TX.
- RJ45-side pins also show waveform after the cable is connected.
- Wireshark can capture board-originated raw Ethernet frames after removing the
  PF5/ETH_CLK coupling path.
- ThreadX + NetX Duo code is generated and configured for a static IPv4 ping
  test.

### Still Not Solved

- NetX Duo ping currently sees PC-originated ARP requests in Wireshark, but the
  board has not yet replied. The next flashed image includes RX-path diagnostics
  and the NetX driver broadcast-filter fix.
- Link speed/duplex selected by the generated RTL8211 driver should be checked
  in the first NetX run. If gigabit is unstable, temporarily disable
  `ETH_PHY_1000MBITS_SUPPORTED` or force 100M during bring-up.
- `TXDLY` strap is still observed disabled while `RXDLY` is enabled. This does
  not block PHY autonegotiation, but should be fixed before relying on higher
  RGMII data rates.
- The final production schematic should mark the optional PF5/ETH_CLK path as
  DNP/no-connect when the PHY uses its local 25 MHz crystal.

## Current Hardware Findings

The software and RGMII side now look mostly healthy. The remaining risk is
concentrated around the PHY MDI analog path:

- RTL8211F MDI pins.
- ESD/TVS parts on the differential pairs.
- External magnetics orientation and soldering.
- Transformer center-tap capacitor network.
- Cable-side Bob Smith termination.
- RJ45 footprint/pin mapping.

The external transformer shown in the schematic is pin-compatible with the
provided SQ24015-1P G style 24-pin gigabit magnetic module. Measuring low
resistance between P/N of the same transformer winding is normal when power is
off, because the transformer winding has low DC resistance. Compare all four
pairs; a single pair that is much lower than the others may indicate a solder
bridge, shorted ESD part, or damaged winding.

## Recommended Next Checks

1. Verify the external magnetics part orientation, especially pin 1.
2. Compare power-off resistance of all four MDI pairs:
   - P/N on the same side should be low and similar across channels.
   - PHY side to cable side should be open at DC.
   - P/N to GND should not be a permanent low resistance.
3. Temporarily depopulate the active-pair TVS/ESD parts for D1/D2 and retest
   autonegotiation.
4. Change PHY-side center-tap capacitors from 10 nF to 100 nF for D1/D2 first,
   then all four pairs if the result improves.
5. Review the cable-side Bob Smith network. For a bench test, bypass the series
   1 nF capacitors between each 75 ohm resistor and the common node, and keep
   the final common-node capacitor to chassis/PGND.
6. Retest through a switch or direct PC link and check whether:
   - NetX prints `NetX link: up`.
   - The PC ARP table learns `02:00:00:00:00:01` for `192.168.1.50`.
   - `ping 192.168.1.50` receives replies.
   - Wireshark sees ARP/ICMP frames from `02:00:00:00:00:01`.

## Build

From this variant directory:

```sh
make -C Makefile/FSBL
make -C Makefile/Appli
```

Or use the VS Code task:

```text
Build: All
```

## Sign And Flash

STM32N6 external Flash boot uses trusted images, not the raw `.bin` files.

VS Code tasks:

```text
Sign: All
Flash: BootChain EXT (FSBL->Appli)
```

Flash addresses from `.vscode/settings.json`:

```text
FSBL  trusted image -> 0x70000000
Appli trusted image -> 0x70100000
```

## CubeMX Regeneration Notes

After CubeMX regeneration, re-check these items:

- `ETH1` must stay enabled in `RGMII` mode.
- `PF5 = ETH1_CLK` must not be initialized as ETH alternate function on this
  board. If CubeMX re-adds PF5, manually remove `GPIO_PIN_5` from the ETH GPIOF
  init/deinit masks or clear the PF5 assignment in CubeMX while keeping ETH1
  enabled.
- Re-apply the ETH1 100 MHz kernel clock change in `Appli/Core/Src/eth.c` if
  CubeMX overwrites it.
- Re-apply the manual GPIO alignment if CubeMX overwrites it:
  GPIOF/GPIOG RGMII pins use pull-up, PF0 uses pull-up and medium speed, ETH1
  IRQ priority is `7`.
- Keep `HAL_GetTick()` overridden in `Appli/Core/Src/app_timebase.c` unless CubeMX is
  configured to use a hardware timer such as TIM6 as the HAL time base.
- Keep `NX_IP_PERIODIC_RATE` aligned with `TX_TIMER_TICKS_PER_SECOND`
  (`1000` in this project).
- Keep `ETH_PHY_1000MBITS_SUPPORTED` defined either in the Appli Makefile or in
  `Appli/NetXDuo/Target/nx_stm32_eth_config.h`. The config header uses an
  `#ifndef` guard so the Makefile definition can coexist with it.
- Re-apply the local NetX Ethernet driver broadcast-filter fix if middleware is
  restored by CubeMX or copied from a fresh Cube package:
  `FilterConfig.BroadcastFilter = DISABLE`, followed by
  `HAL_ETH_SetMACFilterConfig(&eth_handle, &FilterConfig)` during hardware
  initialization.
- Keep the simplified official-style NetX link-management thread in
  `Appli/NetXDuo/App/app_netxduo.c`; CubeMX regeneration may remove it from the
  user-code sections if the file is regenerated unexpectedly.
- Check `Makefile/FSBL/Makefile` after regeneration. FSBL must still compile
  `FSBL/Core/Src/extmem.c` and the `STM32_ExtMem_Manager` sources/includes,
  otherwise `stm32_extmem.h` will be missing and FSBL will not build.
- Keep the Ethernet diagnostic/test code in the split helper modules and re-add
  them to `Makefile/Appli/Makefile` after CubeMX regeneration:
  `app_console.c`, `app_timebase.c`, `eth_diagnostics.c`,
  `eth_bringup_tests.c`.
- Keep the RIF ETH1 master setup and CPUAXI SRAM region access for ETH DMA.
- Rebuild both FSBL and Appli after regeneration.

## Validation Checklist

1. Build `FSBL` and `Appli` successfully.
2. Sign trusted binaries.
3. Flash FSBL to `0x70000000` and Appli to `0x70100000`.
4. Boot from external Flash.
5. Confirm USART3 prints the startup line and Ethernet diagnostics.
6. If low-level PHY diagnostics are re-enabled, confirm MDIO reads PHY ID
   `0x001C / 0xC916`.
7. Confirm ETH kernel clock is 100 MHz.
8. Confirm PF5 is not driving the PHY crystal/EXT_CLK node.
9. Confirm NetX Duo prints `NetX Duo init done`.
10. Confirm NetX Duo prints `NetX link: up`.
11. Configure the PC wired adapter to `192.168.1.10/24`.
12. Confirm Wireshark sees ARP traffic for `192.168.1.50`.
13. Confirm `ping 192.168.1.50` receives replies.

## Latest Debug Notes

2026-06-29:

- After enabling NetX Duo, one flashed image stopped after printing only
  `BOOT: USART ` and the LED stayed on.
- The failure point is before SPI, ETH, and NetX startup. It means the Appli
  image is entered and USART3 init has run, but the early blocking UART print
  can stall the boot path.
- `App_Print()` in `Appli/Core/Src/main.c` and `NetXDuo_Print()` in
  `Appli/NetXDuo/App/app_netxduo.c` now use bounded direct USART polling
  instead of `HAL_UART_Transmit(..., HAL_MAX_DELAY)`.
- Rebuilt with `make -C Makefile/Appli -j32`.
- Regenerated
  `Makefile/Appli/build/fsbl_appli_Ethernet_Appli-trusted.bin`.
- The next diagnostic image expands the UART polling wait and adds short
  boot-stage markers before the verbose strings: `U2`, `S2`, `E2`, and `N2`.
- The following diagnostic image sends one byte at a time and waits for `TC`
  before sending the next byte. Early verbose boot strings were removed, so a
  fresh image should no longer print `BOOT: USART ok` before `S2`.

2026-06-30:

- Compared this project against the official STM32CubeN6
  `STM32N6570-DK/Applications/NetXDuo/Nx_WebServer` example.
- Applied the DK ETH GPIO/IRQ differences that are safe for this board:
  RGMII pull-ups, PF0 medium speed, ETH1 IRQ priority `7`.
- Added `HAL_PWREx_EnableVddIO5()` to match the official global MSP setup.
- Added the ETH1 RIF slave secure attribute, while keeping the Appli's
  secure/non-privileged access model.
- Added `E0`, `R0`, and `R9` boot markers around ETH init and RIF init.
- Built successfully with `make -C Makefile/Appli -j32`.
- Build size after the change: `text=61180`, `data=148`, `bss=90028`.
  This is not close to exhausting CPUAXI SRAM.
- A follow-up boot stopped after `U2`, `S2`, and a single `E`, before the
  `M0` marker inside `MX_ETH1_Init()`. That means the code had not reached ETH
  init yet; it was blocked inside the early marker UART send path.
- Early UART diagnostic writes now wait only for `TXE/TXFNF` before loading the
  next byte and no longer wait for `TC` after every byte. This keeps diagnostic
  output from blocking the boot path.
- The next boot reported `U2`, `S2`, `E0`, then `HF`. This confirms the image
  enters the Appli and reaches the ETH-init boundary, but then takes a
  HardFault before the first ETH marker (`M0`) is observed.
- `Appli/Core/Src/stm32n6xx_it.c` now dumps fault context on HardFault:
  `CFSR`, `HFSR`, `DFSR`, `AFSR`, `BFAR`, `MMFAR`, `EXR`, and the stacked
  `R0/R1/R2/R3/R12/LR/PC/xPSR`. Use `S_PC` with `addr2line` against
  `Makefile/Appli/build/fsbl_appli_Ethernet_Appli.elf` to identify the exact
  faulting line.
- The temporary single-byte `c/d` boot probes around `MX_ETH1_Init()` were
  removed so the next diagnostic image only reports normal stage markers and
  fault context.
- The captured HardFault decoded to `S_PC=0x34000944`, which is
  `_tx_timer_interrupt` in `tx_timer_interrupt.S`. `CFSR=0x00008200` indicates
  a precise BusFault while ThreadX tried to dereference `_tx_timer_current_ptr`;
  the stacked `R0` was `0x00000000`.
- Root cause for this crash: `main()` enabled global interrupts before
  `MX_ThreadX_Init()`. SysTick entered the ThreadX handler before the ThreadX
  timer list had been initialized. The early `__enable_irq()` was removed;
  ThreadX now enables interrupts from its own low-level startup path.
- A repeat boot showed the same `E0 -> HF` signature, which means SysTick was
  already able to interrupt before that removed `__enable_irq()` line was
  reached. The Appli now disables IRQs and clears SysTick as soon as `main()`
  starts, then clears the HAL-created SysTick again immediately after
  `HAL_Init()`. ThreadX still re-enables and reconfigures SysTick inside
  `_tx_initialize_low_level()`.
- After that fix the boot reached `M9`, `E2`, `R9`, `N2`, and
  `NetX Duo init done`. This proves the early boot, ETH HAL init, RIF setup,
  and NetX object creation all complete.
- A later observation with the normal status-thread priority reached
  `NetX Duo static IPv4: 192.168.1.50/24`, `NetX link: up`, and periodic
  `NetX stats`. This confirms the ThreadX scheduler and the NetX status thread
  are running.
- Current NetX counters are still all zero until external traffic reaches the
  interface. During ping validation, `ARP req rx` should increment first; after
  ARP resolution, `ARP resp tx`, `IP rx`, and ICMP counters should change.
- `main.c` was cleaned up so it only calls high-level helpers during startup.
  UART console helpers, ThreadX/HAL tick glue, Ethernet clock diagnostics, and
  reserved bring-up test hooks now live in separate `.c/.h` pairs under
  `Appli/Core`.
- Rebuilt Appli with `make -C Makefile/Appli -j32`; build succeeded with
  `text=61956`, `data=148`, `bss=90028`.
- Regenerated
  `Makefile/Appli/build/fsbl_appli_Ethernet_Appli-trusted.bin`.
- PC-side wired Ethernet is now verified as `192.168.1.10/24`, link up at
  1 Gbps. Wireshark on the correct `以太网` interface shows repeated ARP
  requests: `Who has 192.168.1.50? Tell 192.168.1.10`.
- Because NetX statistics stayed at zero while those ARP broadcasts were on the
  wire, the current investigation is focused on the ETH RX path before NetX IP
  processing.
- Added periodic ETH RX runtime diagnostics: ETH IRQ count, HAL state/error,
  negotiated MAC speed/duplex/port selection, `MACPFR`, `MACRXTXSR`, DMA/MTL
  RX registers, MMC RX counters, and the four RX descriptors.
- Confirmed the generated NetX Ethernet driver provides the HAL RX callbacks
  (`HAL_ETH_RxAllocateCallback`, `HAL_ETH_RxLinkCallback`, and
  `HAL_ETH_RxCpltCallback`), so missing RX allocation callbacks are not the
  current suspect.
- Found a likely ARP blocker in the ST NetX Ethernet driver configuration:
  `BroadcastFilter = ENABLE` maps to the HAL `DBF` bit, meaning "disable
  broadcast frames". The local driver now uses `BroadcastFilter = DISABLE` and
  applies the filter during hardware initialization.
- Rebuilt Appli with `make -C Makefile/Appli -j32`; build succeeded with
  `text=63300`, `data=148`, `bss=90036`.
- Regenerated
  `Makefile/Appli/build/fsbl_appli_Ethernet_Appli-trusted.bin`.
- Rechecked the official STM32N6570-DK `Nx_WebServer` application and ported its
  NetX link-thread pattern into this static-IP test. The new
  `NetXDuo_LinkThreadEntry()` watches `NX_IP_LINK_ENABLED` and issues
  `NX_LINK_ENABLE` / `NX_LINK_DISABLE` direct commands on cable transitions.
  The thread starts in a logical "link down" state so a board booted with the
  cable already connected still logs and attempts the first `NX_LINK_ENABLE`.
- Rebuilt Appli with `make -C Makefile/Appli -j32`; build succeeded with
  `text=64016`, `data=148`, `bss=90212`.
- The latest ping test proves the PC is transmitting ARP broadcasts on the
  wired interface, but the board's NetX/IP statistics, ETH IRQ count, and MMC
  RX counters stay at zero. This points before NetX ARP processing: frames are
  not reaching the STM32 ETH MAC RX path.
- The current diagnostic image intentionally limits RTL8211F auto-negotiation
  advertisement to 100M full duplex with `LIMIT_RTL8211F_TO_100M_FULL=1`.
  This keeps auto-negotiation enabled, but prevents a 1G link while testing.
- Periodic ETH diagnostics now also print PHY page-0 registers (`BMCR`,
  `BMSR`, `ANAR`, `ANLPAR`, `ANER`, `GBCR`, `GBSR`) plus page `0x0A43`
  `PHYSR1`, so the actual PHY link mode can be checked against the MAC
  `Speed`, `DuplexMode`, and `PortSelect` fields.
- Expected result for this image: the PC wired adapter should report 100 Mbps,
  and the board log should show 100M full duplex. If ARP RX counters start
  moving at 100M, focus next on 1G RGMII timing/delay/signal integrity. If RX
  counters remain zero at 100M, keep investigating the PHY-to-STM32 RGMII RX
  path, pin mux, or receive clock/data timing.
- The 100M diagnostic boot confirmed the forced/limited negotiation path:
  `PHY PA43 PHYSR1=0x301E`, `MAC speed=0x00004000`, and
  `MAC portselect=0x00000001`. This is 100M full duplex at both PHY and MAC.
- Two PC ping attempts still produced no NetX RX activity and no ETH MAC RX
  activity: `ARP req rx=0`, `IRQ count=0`, and the MAC MMC RX counters stayed
  at zero. This keeps the active suspect before NetX and before DMA
  descriptors: the PHY-to-STM32 RGMII RX side is still not delivering frames to
  the MAC.
- Default serial output is now compact. Early boot markers (`U2`, `S2`,
  `E0`, `M0...`, `P0...`) and the full ETH register dump were removed from the
  normal path. The status thread now prints one `NX:` summary line and one
  `ETH:` summary line every few seconds. The full ETH register dump remains in
  `Ethernet_PrintRxRuntimeDebug()` and can be re-enabled by setting
  `APP_NETX_VERBOSE_DIAG` to `1` in `Appli/NetXDuo/App/app_netxduo.c`.

Expected compact UART output:

- `FSBL->Appli NetX Ethernet start` means startup reached the NetX phase.
- `NetX init OK` means NetX objects, ARP, ICMP, and status/link threads were
  created successfully.
- `IP: 192.168.1.50/24` is the static board IPv4 configuration.
- `NetX link: up` / `NetX link: down` reports NetX link state.
- `NX:` is the compact NetX counter line. Watch `arp_req`, `arp_resp`,
  `ip_rx`, and `drop`.
- `ETH:` is the compact ETH/MAC/PHY line. Watch `irq`, `mac_rx`, `crc`,
  `bmsr`, `physr1`, `speed`, and `ps`.
- HardFault/NMI/MemManage/BusFault/UsageFault handlers still print full fault
  context when a fault occurs.
- After checking the board, PF7 / `ETH1_RGMII_RX_CLK` was found to have both an
  external pull-up and pull-down fitted, producing about 1.7 V. The pull-up was
  removed, but no RX clock waveform was observed on either the STM32 PF7 side
  or the RTL8211F `RXCLK/PHYAD1` side. This is now the strongest RX-path clue:
  the PHY link can be up over MDI/MDIO, but without RGMII RXCLK the STM32 ETH
  MAC cannot receive ARP frames, which matches `irq=0`, `mac_rx=0`, and NetX RX
  counters staying at zero.
- R118/PF5 is a separate issue from PF7/RXCLK. R118 connected the MCU
  `ETH_CLK/PF5` net to the PHY crystal/EXT_CLK node and could corrupt the PHY
  25 MHz reference if populated or driven. With R118 removed, PF5 no longer
  explains a missing RGMII RXCLK. At 100M link speed, the expected RGMII RXCLK
  on PF7 is 25 MHz; at 1G it is 125 MHz; at 10M it is 2.5 MHz.
- Because RTL8211F `RXCLK` is multiplexed with the `PHYAD1` strap, any hardware
  change on PF7/RXCLK should be followed by a full power cycle or a PHY reset
  sequence that re-latches straps. The board strap target is still MDIO address
  `0x01` (`PHYAD0=1`, `PHYAD1=0`, `PHYAD2=0`). A previous diagnostic scan
  started at address `0x00` and could report an address-0 false/mirrored
  response. The diagnostic scan now matches the ST RTL8211 component driver:
  it first verifies expected address `0x01`, then scans `1..31`.
- After removing the PF7/RXCLK pull-up and power cycling, the link still
  reports 100M full duplex (`bmsr=0x79AD`, `physr1=0x301E`), while `irq=0`,
  `mac_rx=0`, and NetX RX counters remain zero. A follow-up image now
  explicitly sets RTL8211F page `0x0A43` `PHYCR2.RXC_ENABLE` and prints
  `phycr2=` in the compact `ETH:` line. If `phycr2` includes bit `0x0002` but
  PF7/RXCLK still has no 25 MHz waveform at a 100M link, continue with hardware
  checks around the PHY `RXCLK/PHYAD1` pin, reset/strap network, and the 22 ohm
  series resistor path to PF7.
- 2026-07-01 raw TX recheck image:
  `Makefile/Appli/Makefile` now defines `APP_ETH_RAW_TX_TEST=1`.
  `Ethernet_BringupTests_BeforeNetX()` enters a raw broadcast TX loop and does
  not start ThreadX/NetX. The frame is EtherType `0x88B5`, source MAC
  `02:00:00:00:00:01`, and payload prefix `STM32N6 RAW TX SEQ=`.
- The raw TX test keeps `ETH_TxPacketConfigTypeDef.pData = NULL` and does not
  call `HAL_ETH_ReleaseTxPacket()`. The generated NetX Ethernet driver owns
  `HAL_ETH_TxFreeCallback()` and expects an `NX_PACKET` pointer there; passing a
  bare frame buffer into that callback caused a UsageFault after the first TX.
- After flashing the fixed raw TX image, serial output shows continuous
  `RAW TX seq` progress (`0x00000000`, `0x00000040`, `0x00000080`,
  `0x000000C0`, ...), and `MMCTPCGR/TX good` advances with the sequence.
  Wireshark on the PC captures continuous `0x88B5` broadcast frames from
  `02:00:00:00:00:01`. This confirms the STM32 TX path through the RTL8211F,
  magnetics/RJ45, cable, and PC receive side is working.
- PF7 / `ETH1_RGMII_RX_CLK` is now initialized separately with `GPIO_NOPULL`
  instead of being included in the GPIOF pull-up group. This keeps the RX clock
  node clean for the next RX-path measurement. It is not required for the raw
  TX direction, but avoids masking the PF7 hardware observation.

## Guardrails

- This variant is now a first NetX Duo bring-up project, not a complete product
  network application.
- Keep the raw TX and MDIO diagnostics guarded in the code until NetX ping is
  stable across direct-PC and switch tests.
- Keep changes scoped to this variant; other FSBL/Appli variants may have
  unrelated work in progress.
