# FSBL Appli LRUN Clean Baseline

This variant is the current clean two-stage development baseline:

```text
FSBL -> Appli
```

Runtime behavior:

- FSBL boots from external Flash at `0x70000000`.
- FSBL loads Appli from external Flash at `0x70100000`.
- Appli initializes GPIO and USART3.
- PO1 LED runs a breathing pattern.
- USART3 prints a startup line and a 1-second heartbeat.

Expected UART output:

```text
FSBL->Appli start
FSBL->Appli heartbeat
FSBL->Appli heartbeat
...
```

UART:

```text
USART3
TX: PD8
RX: PD9
115200 8N1
```

Disabled by design:

- FSBL PSRAM diagnostics: `FSBL_PSRAM_DIAG_LEVEL == 0`.
- XSPI1 PSRAM initialization in FSBL.
- Appli PSRAM sanity and 4 MiB buffer tests.
- SPI4/GPDMA initialization in Appli.
- AD7606 SPI DMA receiver startup.

Build:

```powershell
make -C Makefile\FSBL
make -C Makefile\Appli
```

Signed outputs:

```text
Makefile/FSBL/build/fsbl_appli_lrun_FSBL-trusted.bin
Makefile/Appli/build/fsbl_appli_lrun_Appli-trusted.bin
```

Use this baseline for new interface development. Re-enable other subsystems one
at a time and keep the UART heartbeat/LED breathing pattern as basic health
signals.

Historical note:

- `2904513` keeps the last known stable PSRAM mmap diagnostic version.
- `0c9a5c9` introduced this clean LED/USART heartbeat baseline.

