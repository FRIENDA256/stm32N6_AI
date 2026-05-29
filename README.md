# stm32N6_AI

Baseline STM32N6570-DK Makefile project for VS Code.

This repository contains a three-stage external flash boot chain derived from
STM32CubeN6 Template_Isolation_LRUN:

- FSBL
- AppliSecure
- AppliNonSecure

The current validated baseline boots from external flash, loads Secure and
NonSecure images, switches into NonSecure state, and toggles LED PO1 every
500 ms from `AppliNonSecure/Core/Src/main.c`.

## Build

```powershell
make -C Makefile/FSBL
make -C Makefile/AppliSecure
make -C Makefile/AppliNonSecure
```

## Flash Layout

```text
FSBL            0x70000000
AppliSecure     0x70100000
AppliNonSecure  0x70180000
```

VS Code tasks in `.vscode/tasks.json` provide build, signing, and external
flash programming commands for the local STM32CubeProgrammer/OpenOCD setup.
