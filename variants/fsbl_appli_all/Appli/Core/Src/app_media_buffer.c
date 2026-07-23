/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_media_buffer.c
  * @brief   Shared high-throughput media buffer for camera and Tiny1C data.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "app_media_buffer.h"

#include <stddef.h>

/*
 * Keep this buffer in the normal high SRAM BSS flow. Its link position is the
 * same fast media area formerly used by the IMX219 frame buffer, while the
 * first three slots are reused by Tiny1C when the camera is not streaming.
 */
static uint8_t AppMediaFrameBuffer[APP_MEDIA_BUFFER_BYTES]
  __attribute__((aligned(8192)));

uint8_t *AppMediaBuffer_GetCamera(void)
{
  return AppMediaFrameBuffer;
}

uint32_t AppMediaBuffer_GetCameraSize(void)
{
  return (uint32_t)sizeof(AppMediaFrameBuffer);
}

uint8_t *AppMediaBuffer_GetTiny1CSlot(uint32_t slot_index)
{
  if (slot_index >= APP_MEDIA_TINY1C_FRAME_SLOTS)
  {
    return NULL;
  }

  return &AppMediaFrameBuffer[slot_index * APP_MEDIA_TINY1C_FRAME_BYTES];
}
