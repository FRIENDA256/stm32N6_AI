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
#define APP_IR_CAPTURE_IDLE_TEMP_FPS 2U
#define APP_IR_CAPTURE_SNAPSHOT_FPS 20U

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
  uint32_t external_ram_ready;
  uint32_t external_ram_magic;
  uint32_t external_ram_status;
  uint32_t external_ram_step;
  uint32_t external_ram_error;
  uint32_t external_ram_hal_error;
  uint32_t snapshot_publish_count;
  uint32_t snapshot_publish_miss_count;
  uint32_t snapshot_copy_count;
  uint32_t snapshot_copy_last_ms;
  uint32_t snapshot_copy_max_ms;
  uint32_t snapshot_copy_total_ms;
  uint8_t last_command;
} App_IRCapture_Status_t;

typedef struct
{
  const uint8_t *data;
  uint32_t length;
  uint32_t sequence;
  uint32_t timestamp_ms;
  uint32_t crc32;
  uint32_t slot_index;
  uint8_t command;
} App_IRCapture_FrameLease_t;

UINT App_IRCapture_Start(TX_BYTE_POOL *byte_pool);
UINT App_IRCapture_Pause(ULONG wait_ticks);
void App_IRCapture_Resume(void);
void App_IRCapture_GetStatus(App_IRCapture_Status_t *status);
UINT App_IRCapture_AcquireLatestFrame(uint8_t command,
                                      App_IRCapture_FrameLease_t *lease);
void App_IRCapture_ReleaseFrame(const App_IRCapture_FrameLease_t *lease);
/* Raise (active=1) or lower (active=0) the capture thread priority between
   PRIORITY_ACTIVE (13, IR mode) and PRIORITY_IDLE (13, AD+AI mode). */
UINT App_IRCapture_SetActive(uint8_t active);

#ifdef __cplusplus
}
#endif

#endif /* APP_IR_CAPTURE_H */
