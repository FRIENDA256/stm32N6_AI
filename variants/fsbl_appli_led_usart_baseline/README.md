# FSBL Appli LED USART Baseline

This archived variant keeps a minimal two-stage secure boot chain:

```text
FSBL -> Appli
```

Scope:

- FSBL initializes GPIO, external Flash path through XSPI2/EXTMEM, then jumps to Appli.
- Appli initializes GPIO and USART3 only.
- Appli prints `FSBL->Appli LED USART baseline start` once.
- Appli toggles the board LED and prints `FSBL->Appli heartbeat` every second.

Removed from this baseline:

- PSRAM direct or indirect read/write tests.
- XSPI1 PSRAM memory-mapped bring-up in application code.
- SPI4, GPDMA, AD7606 DMA receiver, and external RAM diagnostics.

Build from this directory with:

```sh
make -C Makefile/FSBL
make -C Makefile/Appli
```

