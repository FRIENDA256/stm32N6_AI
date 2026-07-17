/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_ir_capture.h
  * @brief   Scheduled Tiny1C image and temperature acquisition.
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef APP_IR_CAPTURE_H
#define APP_IR_CAPTURE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "tiny1c_debug_driver.h"
#include "tx_api.h"

#include <stdint.h>

#define APP_IR_CAPTURE_IMAGE_FPS 0U
#define APP_IR_CAPTURE_TEMP_FPS  20U

typedef struct
{
  uint32_t initialized;
  uint32_t running;
  uint32_t paused;
  uint32_t active;
  uint32_t image_count;
  uint32_t temp_count;
  uint32_t image_capture_total_ms;
  uint32_t temp_capture_total_ms;
  uint32_t capture_total_ms;
  uint32_t capture_error_count;
  uint32_t deadline_miss_count;
  uint32_t image_sequence;
  uint32_t temp_sequence;
  uint32_t last_image_ms;
  uint32_t last_temp_ms;
  uint32_t last_capture_ms;
  uint32_t max_capture_ms;
  uint32_t schedule_elapsed_ms;
  uint32_t target_image_fps;
  uint32_t target_temp_fps;
  uint32_t spi_clock_hz;
  uint8_t last_command;
} App_IRCapture_Status_t;

UINT App_IRCapture_Start(TX_BYTE_POOL *byte_pool);
UINT App_IRCapture_Pause(ULONG wait_ticks);
void App_IRCapture_Resume(void);
void App_IRCapture_GetStatus(App_IRCapture_Status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* APP_IR_CAPTURE_H */
