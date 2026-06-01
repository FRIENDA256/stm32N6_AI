# stm32N6_AI

Baseline STM32N6570-DK Makefile project for VS Code.

This repository contains a three-stage external flash boot chain derived from
STM32CubeN6 Template_Isolation_LRUN:

- FSBL
- AppliSecure
- AppliNonSecure

The current validated baseline boots from external flash, loads Secure and
NonSecure images, switches into NonSecure state, toggles LED PO1 every 500 ms,
and outputs a USART3 heartbeat from `AppliNonSecure/Core/Src/main.c`.

## Build

```powershell
make -C Makefile/AppliSecure
make -C Makefile/FSBL
make -C Makefile/AppliNonSecure
```

`AppliNonSecure` links against the Secure CMSE import library, so build
`AppliSecure` before `AppliNonSecure`.

## Flash Layout

```text
FSBL            0x70000000
AppliSecure     0x70100000
AppliNonSecure  0x70180000
```

VS Code tasks in `.vscode/tasks.json` provide build, signing, and external
flash programming commands for the local STM32CubeProgrammer/OpenOCD setup.

## Current Test Signals

- LED PO1 toggles every 500 ms in NonSecure main loop.
- USART3 uses PD8/PD9 at 115200 8N1.
- USART3 prints `STM32N6_AI AppNS heartbeat` periodically after AppNS starts.

## Notes

- [STM32N6 VSCode Makefile bootchain debug notes](docs/STM32N6_VSCODE_MAKEFILE_BOOTCHAIN_DEBUG_NOTES.md)
