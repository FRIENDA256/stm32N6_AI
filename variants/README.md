# Project Variants

This directory keeps alternative project layouts without disturbing the current
TrustZone boot-chain baseline at the repository root.

## Current Root Project

The repository root is the verified TrustZone boot chain:

```text
FSBL -> AppliSecure -> AppliNonSecure
```

Keep this layout in place as the known-good reference for external Flash boot,
Secure/NonSecure handoff, USART3, LED, SPI4 DMA, and AD7606 communication.

## fsbl_appli_lrun

Use `variants/fsbl_appli_lrun/` for the current simplified boot chain and NPU
inference baseline:

```text
FSBL -> Appli + STM32N6 NPU self-test
```

This is the current active development baseline. It follows the official
`VENC_RTSP_Server` style:

- FSBL stored in external Flash at `0x70000000`
- Appli stored in external Flash at `0x70100000`
- FSBL loads Appli into internal RAM for LRUN
- Appli initializes GPIO and USART3
- PO1 LED runs a breathing pattern
- USART3 prints startup, NPU self-test, and 1-second heartbeat logs
- Appli runs `tiny_temporal_mixer_8ch_int8` on the STM32N6 NPU with a fixed
  8-channel x 1024 INT8 test input
- NPU weights are stored in xSPI2 Flash at `0x71000000`
- NPU self-test success is `NPU output raw bytes=00 05 D9 8D` and
  `NPU AI self-test OK`

Disabled in this baseline:

- FSBL PSRAM diagnostics
- XSPI1 PSRAM memory-mapped bring-up
- Appli PSRAM sanity and large-buffer tests
- SPI4, GPDMA, AD7606 receiver startup

Use this variant as the starting point for AI deployment work and for adding
other interface features around the verified NPU runtime. The last known stable
PSRAM diagnostic version is available in git history at commit `2904513`. The
pure LED/USART baseline is available at commit `0c9a5c9` and in
`variants/fsbl_appli_led_usart_baseline/`.

Do not create a nested Git repository inside this directory.

## fsbl_appli_led_usart_baseline

Use `variants/fsbl_appli_led_usart_baseline/` as the archived minimal
FSBL-to-Appli baseline:

```text
FSBL -> Appli
```

This baseline keeps only the handoff path, LED heartbeat, and USART3 logging.
It intentionally excludes PSRAM tests, SPI4, GPDMA, AD7606, and application-side
external RAM validation so it can be used as a clean bring-up reference.
