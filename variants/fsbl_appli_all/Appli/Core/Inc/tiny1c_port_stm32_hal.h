#ifndef TINY1C_PORT_STM32_HAL_H
#define TINY1C_PORT_STM32_HAL_H

#include "spi.h"
#include "tiny1c_debug_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

extern tiny1c_t g_tiny1c;

typedef struct
{
  uint32_t ok_count;
  uint32_t error_count;
  uint32_t total_ms;
  uint32_t max_ms;
  uint32_t total_jumps;
  uint32_t max_jumps;
  uint32_t max_delta;
  uint32_t last_crc32;
  uint32_t last_hal_error;
} Tiny1C_STM32_SpiModeStats_t;

typedef struct
{
  uint32_t iterations;
  uint32_t test_hz;
  uint32_t restored_hz;
  uint32_t switch_hal_status;
  uint32_t priority_hal_status;
  uint32_t restore_hal_status;
  Tiny1C_STM32_SpiModeStats_t blocking;
  Tiny1C_STM32_SpiModeStats_t dma;
} Tiny1C_STM32_SpiTestResult_t;

typedef struct
{
  uint32_t transfer_count;
  uint32_t dma_start_error_count;
  uint32_t dma_wait_timeout_count;
  uint32_t dma_callback_error_count;
  uint32_t blocking_error_count;
  uint32_t last_failure_reason;
  uint32_t last_hal_status;
  uint32_t last_hal_error;
  uint32_t last_error_ms;
  uint32_t last_transfer_len;
  uint32_t last_spi_sr;
  uint32_t last_spi_cr1;
  uint32_t last_spi_cr2;
  uint32_t last_spi_cfg1;
  uint32_t last_spi_state;
  uint32_t last_rx_remaining;
  uint32_t last_tx_remaining;
  uint32_t last_rx_dma_state;
  uint32_t last_tx_dma_state;
  uint32_t auto_suspend_resume_count;
} Tiny1C_STM32_DmaDiagnostics_t;

void Tiny1C_STM32_GpioInit(void);
tiny1c_status_t Tiny1C_STM32_Init(void);
tiny1c_status_t Tiny1C_STM32_ProcessCommand(uint8_t command);
tiny1c_status_t Tiny1C_STM32_CaptureFrame(uint8_t frame_command);
tiny1c_status_t Tiny1C_STM32_CaptureFrameBaseline(uint8_t frame_command);
tiny1c_status_t Tiny1C_STM32_CaptureFrameQuiet(uint8_t frame_command);
tiny1c_status_t Tiny1C_STM32_RestartPreview(void);
tiny1c_status_t Tiny1C_STM32_RunSpi50Test(uint32_t iterations,
                                          Tiny1C_STM32_SpiTestResult_t *result);
tiny1c_status_t Tiny1C_STM32_RunSpi50DmaTest(uint32_t iterations,
                                             Tiny1C_STM32_SpiTestResult_t *result);
tiny1c_status_t Tiny1C_STM32_GetLatestFrame(const uint8_t **data,
                                            uint32_t *len,
                                            uint8_t *frame_command,
                                            uint32_t *crc32);
uint32_t Tiny1C_STM32_GetSpiClockHz(void);
void Tiny1C_STM32_GetDmaDiagnostics(Tiny1C_STM32_DmaDiagnostics_t *diagnostics);
void Tiny1C_STM32_TxRxCpltCallback(SPI_HandleTypeDef *hspi);
void Tiny1C_STM32_ErrorCallback(SPI_HandleTypeDef *hspi);
void Tiny1C_STM32_SuspendCallback(SPI_HandleTypeDef *hspi);
void Tiny1C_STM32_PrintBootMessage(void);

#ifdef __cplusplus
}
#endif

#endif /* TINY1C_PORT_STM32_HAL_H */
