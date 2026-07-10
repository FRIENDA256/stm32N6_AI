/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_camera_imx219.h
  * @brief   Minimal IMX219/DCMIPP bring-up helpers for TCP-command testing.
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef APP_CAMERA_IMX219_H
#define APP_CAMERA_IMX219_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32n6xx_hal.h"
#include <stddef.h>
#include <stdint.h>

#define APP_CAMERA_WIDTH             640U
#define APP_CAMERA_HEIGHT            480U
#define APP_CAMERA_BYTES_PER_PIXEL   2U
#define APP_CAMERA_FRAME_BYTES       (APP_CAMERA_WIDTH * APP_CAMERA_HEIGHT * APP_CAMERA_BYTES_PER_PIXEL)

typedef struct
{
  uint32_t stage;
  HAL_StatusTypeDef hal_status;
  uint32_t hal_error;
  int32_t imx_status;
  uint32_t sensor_id;
  uint32_t width;
  uint32_t height;
} AppCamera_Result_t;

typedef struct
{
  uint32_t initialized;
  uint32_t configured;
  uint32_t dcmipp_configured;
  uint32_t sensor_configured;
  uint32_t streaming;
  uint32_t sensor_id;
  uint32_t width;
  uint32_t height;
  uint32_t frame_bytes;
  uint32_t buffer_addr;
  uint32_t frame_count;
  uint32_t vsync_count;
  uint32_t sof_count;
  uint32_t eof_count;
  uint32_t pipe_error_count;
  uint32_t csi_error_count;
  uint32_t hw_frame_count;
  uint32_t dcmipp_error;
  uint32_t p1sr;
  uint32_t sr0;
  uint32_t sr1;
  uint32_t csi_err1;
  uint32_t csi_err2;
  uint16_t mode_reg;
  uint16_t line_reg;
  uint16_t frame_reg;
} AppCamera_Status_t;

HAL_StatusTypeDef AppCamera_Probe(AppCamera_Result_t *result);
HAL_StatusTypeDef AppCamera_Config(AppCamera_Result_t *result);
HAL_StatusTypeDef AppCamera_ConfigDcmippOnly(AppCamera_Result_t *result);
HAL_StatusTypeDef AppCamera_ConfigSensorOnly(AppCamera_Result_t *result);
HAL_StatusTypeDef AppCamera_Start(AppCamera_Result_t *result);
HAL_StatusTypeDef AppCamera_Stop(AppCamera_Result_t *result);
HAL_StatusTypeDef AppCamera_FreezeLatestFrame(AppCamera_Result_t *result);
void AppCamera_GetStatus(AppCamera_Status_t *status);
void AppCamera_GetFrameCounters(uint32_t *frame_count, uint32_t *hw_frame_count);
const uint8_t *AppCamera_GetFrameBuffer(void);
uint32_t AppCamera_GetFrameBufferSize(void);
uint32_t AppCamera_GetFrameBytes(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_CAMERA_IMX219_H */
