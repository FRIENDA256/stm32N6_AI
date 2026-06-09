# STM32N6_AI

This repository contains STM32N6570-DK boot-chain experiments and reusable
project variants.

The current main development baseline is:

```text
variants/fsbl_appli_lrun/
FSBL -> Appli
```

It is intentionally clean for new interface development:

- FSBL boots from external Flash and loads Appli.
- Appli initializes GPIO and USART3.
- PO1 LED runs a breathing pattern.
- USART3 prints startup and 1-second heartbeat logs.
- PSRAM, SPI4, GPDMA, AD7606, and large-buffer diagnostics are disabled on the
  startup path.

Expected UART output:

```text
FSBL->Appli start
FSBL->Appli heartbeat
FSBL->Appli heartbeat
...
```

UART parameters:

```text
USART3
TX: PD8
RX: PD9
115200 8N1
No flow control
```

## Repository Layout

```text
README.md
variants/
  README.md
  fsbl_appli_lrun/                 Current clean FSBL->Appli baseline
  fsbl_appli_led_usart_baseline/   Archived minimal LED/USART baseline
Drivers/
Middlewares/
Makefile/
FSBL/
AppliSecure/
AppliNonSecure/
```

The repository root still contains the older three-stage TrustZone boot-chain
project:

```text
FSBL -> AppliSecure -> AppliNonSecure
```

Keep it as historical reference unless a task explicitly targets that layout.

## Current Baseline

Use `variants/fsbl_appli_lrun/` for new development.

Runtime scope:

- `FSBL/Core/Src/main.c`
  - Initializes GPIO.
  - Initializes XSPI2/EXTMEM for external Flash boot.
  - Does not initialize XSPI1 PSRAM while `FSBL_PSRAM_DIAG_LEVEL == 0`.
  - Calls `BOOT_Application()`.
- `Appli/Core/Src/main.c`
  - Initializes GPIO and USART3.
  - Configures RIF attributes needed by the baseline.
  - Runs PO1 breathing LED.
  - Prints `FSBL->Appli heartbeat` every second.

Disabled by design:

- FSBL PSRAM diagnostics.
- XSPI1 PSRAM memory-mapped validation.
- Appli PSRAM sanity and 4 MiB buffer tests.
- SPI4 / GPDMA / AD7606 receiver startup.

The PSRAM tuning work was validated separately. The last known stable PSRAM
diagnostic version is preserved in git history:

```text
2904513 Stabilize PSRAM mmap test and add heartbeat breathing LED
```

The clean baseline is:

```text
0c9a5c9 Create clean LED USART heartbeat baseline
```

## Build

From the repository root:

```powershell
make -C variants\fsbl_appli_lrun\Makefile\FSBL
make -C variants\fsbl_appli_lrun\Makefile\Appli
```

The generated raw binaries are:

```text
variants/fsbl_appli_lrun/Makefile/FSBL/build/fsbl_appli_lrun_FSBL.bin
variants/fsbl_appli_lrun/Makefile/Appli/build/fsbl_appli_lrun_Appli.bin
```

## Sign

STM32N6 external Flash boot uses STM32 trusted images, not raw binaries.

Use STM32 SigningTool with:

```text
-hv 2.3
-align
-nk
-of 0x80000000
-t fsbl
```

Current signed outputs:

```text
variants/fsbl_appli_lrun/Makefile/FSBL/build/fsbl_appli_lrun_FSBL-trusted.bin
variants/fsbl_appli_lrun/Makefile/Appli/build/fsbl_appli_lrun_Appli-trusted.bin
```

The VS Code tasks under `variants/fsbl_appli_lrun/.vscode/tasks.json` contain
the same build/sign/flash flow.

## Flash Layout

For `variants/fsbl_appli_lrun/`:

```text
FSBL trusted image   0x70000000
Appli trusted image  0x70100000
```

Use STM32CubeProgrammer with the STM32N6570-DK external loader. Do not use a
plain OpenOCD `program <bin> 0x70000000 verify` flow unless an external Flash
bank has been explicitly configured.

For the NPU `tiny_temporal_mixer_8ch_int8` test, the generated weights are not
part of the Appli binary. Program the raw weights into xSPI2 Flash:

```powershell
powershell.exe -ExecutionPolicy Bypass -File tools\flash_npu_weights.ps1
```

This writes:

```text
variants\fsbl_appli_lrun\tiny_temporal_mixer_8ch_int8_atonbuf.xSPI2.raw -> 0x71000000
```

The firmware prints a startup probe for `0x71000000`. `match=1` means the
weights in external Flash match the generated raw file.

## Notes For New Interface Work

This baseline is deliberately quiet. When adding a new peripheral or interface:

- Keep USART3 heartbeat alive as the first health signal.
- Keep PO1 breathing LED alive unless the new feature explicitly owns the LED.
- Enable one subsystem at a time.
- Add diagnostics behind compile-time switches when possible.
- Avoid re-enabling PSRAM/SPI4/AD7606 startup code unless the task requires it.
