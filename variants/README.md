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

Use `variants/fsbl_appli_lrun/` for the new simplified boot chain:

```text
FSBL -> Appli
```

This is the current clean interface-development baseline. It follows the
official `VENC_RTSP_Server` style:

- FSBL stored in external Flash at `0x70000000`
- Appli stored in external Flash at `0x70100000`
- FSBL loads Appli into internal RAM for LRUN
- Appli initializes GPIO and USART3
- PO1 LED runs a breathing pattern
- USART3 prints startup and 1-second heartbeat logs

Disabled in this baseline:

- FSBL PSRAM diagnostics
- XSPI1 PSRAM memory-mapped bring-up
- Appli PSRAM sanity and large-buffer tests
- SPI4, GPDMA, AD7606 receiver startup

Use this variant as the starting point for adding other interface features.
The last known stable PSRAM diagnostic version is available in git history at
commit `2904513`.

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
