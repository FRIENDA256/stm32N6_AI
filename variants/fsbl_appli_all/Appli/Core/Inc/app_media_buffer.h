/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_media_buffer.h
  * @brief   Shared high-throughput media buffer for camera and Tiny1C data.
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef APP_MEDIA_BUFFER_H
#define APP_MEDIA_BUFFER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define APP_MEDIA_BUFFER_BYTES          614400U
#define APP_MEDIA_TINY1C_FRAME_BYTES    98304U
#define APP_MEDIA_TINY1C_FRAME_SLOTS    3U

uint8_t *AppMediaBuffer_GetCamera(void);
uint32_t AppMediaBuffer_GetCameraSize(void);
uint8_t *AppMediaBuffer_GetTiny1CSlot(uint32_t slot_index);

#ifdef __cplusplus
}
#endif

#endif /* APP_MEDIA_BUFFER_H */
