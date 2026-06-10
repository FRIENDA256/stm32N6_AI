# FSBL Appli LRUN NPU Baseline

This variant is the current two-stage STM32N6 NPU inference baseline:

```text
FSBL -> Appli + STM32N6 NPU self-test
```

Runtime behavior:

- FSBL boots from external Flash at `0x70000000`.
- FSBL loads Appli from external Flash at `0x70100000`.
- Appli initializes GPIO and USART3.
- PO1 LED runs a breathing pattern.
- Appli initializes the STM32N6 NPU runtime.
- Appli verifies the NPU weight raw file in xSPI2 Flash.
- Appli runs `tiny_temporal_mixer_8ch_int8` on a fixed 8-channel x 1024 INT8
  input array.
- USART3 prints startup, NPU self-test, and 1-second heartbeat logs.

Expected UART output:

```text
FSBL->Appli start
NPU AI self-test begin
NPU weights full len=98833 sum=0x00BEC661 exp=0x00BEC661 ...
NPU user io set mode=npuram input=0 output=0 in_addr=0x34336000 out_addr=0x34340000
NPU output raw bytes=00 05 D9 8D
NPU top1 index=1 label=class_1 expected=1 raw=5 score=0.017822
NPU AI self-test OK
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

NPU model:

```text
Name: tiny_temporal_mixer_8ch_int8
Generator: STM32CubeMX / X-CUBE-AI with STEdgeAI 2.2, ATON 1.1.1-14
Input: int8[1, 1, 8, 1024], 8192 bytes
Output: int8[1, 4], 4 bytes
Expected raw output: 00 05 D9 8D
Expected Top-1: class_1
```

NPU memory and weights:

```text
xSPI2 weights raw: tiny_temporal_mixer_8ch_int8_atonbuf.xSPI2.raw
xSPI2 weights address: 0x71000000
xSPI2 weights SHA256: CBBDB5C3D09629EA5FB55EEB2E5B58A956FD7B9A439213690926B6F1106E7CF8
NPU activation base: 0x342E0000
NPU user input: 0x34336000
NPU user output: 0x34340000
```

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

Flash the boot chain and NPU weights with the VS Code tasks in `.vscode` or
with the repository scripts. The NPU weights must be programmed separately from
the Appli image.

Use this baseline for AI deployment work. Re-enable other subsystems one at a
time and keep the UART heartbeat/LED breathing pattern as basic health signals.

Historical note:

- `2904513` keeps the last known stable PSRAM mmap diagnostic version.
- `0c9a5c9` introduced this clean LED/USART heartbeat baseline.
