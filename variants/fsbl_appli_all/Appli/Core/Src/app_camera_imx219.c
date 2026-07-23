/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_camera_imx219.c
  * @brief   Minimal IMX219/DCMIPP bring-up helpers for TCP-command testing.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "app_camera_imx219.h"
#include "app_console.h"
#include "app_ir_capture.h"
#include "app_media_buffer.h"
#include "dcmipp.h"
#include "i2c.h"
#include "imx219.h"
#include "main.h"
#include <string.h>

#define APP_CAMERA_LINE_LENGTH        3560U
/* 2200 lines measured about 24 fps on this board; 4400 lines targets 12 fps. */
#define APP_CAMERA_FRAME_LENGTH       4400U
#define APP_CAMERA_OP_PLL_MULT        0x0039U
#define APP_CAMERA_EXPOSURE_LINES     0x0418U
#define APP_CAMERA_ANALOG_GAIN        0x80U
#define APP_CAMERA_DIGITAL_GAIN       0x0100U
#define APP_CAMERA_BLACK_LEVEL        12U
#define APP_CAMERA_CAPTURE_PIPE       DCMIPP_PIPE1
#define APP_CAMERA_CAPTURE_MODE       DCMIPP_MODE_CONTINUOUS
#define APP_CAMERA_VIRTUAL_CHANNEL    DCMIPP_VIRTUAL_CHANNEL0

static IMX219_Object_t AppCameraSensor;
static uint8_t AppCameraBusRegistered;
static uint8_t AppCameraConfigured;
static uint8_t AppCameraDcmippConfigured;
static uint8_t AppCameraSensorConfigured;
static uint8_t AppCameraStreaming;
static uint32_t AppCameraSensorId;
static volatile uint32_t AppCameraFrameCount;
static volatile uint32_t AppCameraVsyncCount;
static volatile uint32_t AppCameraSofCount;
static volatile uint32_t AppCameraEofCount;
static volatile uint32_t AppCameraPipeErrorCount;
static volatile uint32_t AppCameraCsiErrorCount;
static volatile uint32_t AppCameraLastError;

static void AppCamera_ClearResult(AppCamera_Result_t *result);
static HAL_StatusTypeDef AppCamera_SetHalError(AppCamera_Result_t *result,
                                               uint32_t stage,
                                               HAL_StatusTypeDef status);
static HAL_StatusTypeDef AppCamera_SetImxError(AppCamera_Result_t *result, uint32_t stage);
static HAL_StatusTypeDef AppCamera_PowerAndRegister(AppCamera_Result_t *result);
static HAL_StatusTypeDef AppCamera_ConfigDcmipp(AppCamera_Result_t *result);
static HAL_StatusTypeDef AppCamera_ConfigSensor(AppCamera_Result_t *result);
static void AppCamera_CleanFrameBuffer(void);
static void AppCamera_InvalidateFrameBuffer(void);
static void AppCamera_ResetCounters(void);
static void AppCamera_ReadSensorStatus(AppCamera_Status_t *status);
static HAL_StatusTypeDef AppCamera_CheckMediaBuffer(AppCamera_Result_t *result);

static void AppCamera_ClearResult(AppCamera_Result_t *result)
{
  if (result != NULL)
  {
    memset(result, 0, sizeof(*result));
    result->hal_status = HAL_OK;
    result->imx_status = IMX219_OK;
  }
}

static HAL_StatusTypeDef AppCamera_SetHalError(AppCamera_Result_t *result,
                                               uint32_t stage,
                                               HAL_StatusTypeDef status)
{
  if (result != NULL)
  {
    result->stage = stage;
    result->hal_status = status;
    result->hal_error = HAL_DCMIPP_GetError(&hdcmipp);
  }

  return status;
}

static HAL_StatusTypeDef AppCamera_SetImxError(AppCamera_Result_t *result, uint32_t stage)
{
  if (result != NULL)
  {
    result->stage = stage;
    result->hal_status = HAL_ERROR;
    result->imx_status = IMX219_ERROR;
  }

  return HAL_ERROR;
}

static HAL_StatusTypeDef AppCamera_CheckMediaBuffer(AppCamera_Result_t *result)
{
  App_IRCapture_Status_t ir_status;

  App_IRCapture_GetStatus(&ir_status);
  if ((ir_status.running != 0U) && (ir_status.paused == 0U))
  {
    if (result != NULL)
    {
      result->stage = 38U;
      result->hal_status = HAL_BUSY;
    }
    return HAL_BUSY;
  }

  return HAL_OK;
}

static HAL_StatusTypeDef AppCamera_PowerAndRegister(AppCamera_Result_t *result)
{
  HAL_GPIO_WritePin(EN_MODULE_GPIO_Port, EN_MODULE_Pin, GPIO_PIN_SET);
  HAL_Delay(200U);

  if (AppCameraBusRegistered == 0U)
  {
    if (IMX219_RegisterBusIO(&AppCameraSensor, &hi2c1, IMX219_I2C_ADDR_7BIT) != IMX219_OK)
    {
      return AppCamera_SetImxError(result, 1U);
    }

    AppCameraBusRegistered = 1U;
  }

  return HAL_OK;
}

static HAL_StatusTypeDef AppCamera_ConfigDcmipp(AppCamera_Result_t *result)
{
  DCMIPP_BlackLevelConfTypeDef black_level_conf = {0};
  DCMIPP_ExposureConfTypeDef exposure_conf = {0};
  DCMIPP_RawBayer2RGBConfTypeDef raw_bayer_conf = {0};
  HAL_StatusTypeDef status;
  uint32_t stage = 0U;
  uint32_t hal_error = 0U;

  status = App_DCMIPP_DiagnosticInit(&stage, &hal_error);
  if (status != HAL_OK)
  {
    if (result != NULL)
    {
      result->stage = 10U + stage;
      result->hal_status = status;
      result->hal_error = hal_error;
    }
    return status;
  }

  black_level_conf.RedCompBlackLevel = APP_CAMERA_BLACK_LEVEL;
  black_level_conf.GreenCompBlackLevel = APP_CAMERA_BLACK_LEVEL;
  black_level_conf.BlueCompBlackLevel = APP_CAMERA_BLACK_LEVEL;
  status = HAL_DCMIPP_PIPE_SetISPBlackLevelCalibrationConfig(&hdcmipp,
                                                             APP_CAMERA_CAPTURE_PIPE,
                                                             &black_level_conf);
  if (status != HAL_OK)
  {
    return AppCamera_SetHalError(result, 20U, status);
  }
  status = HAL_DCMIPP_PIPE_EnableISPBlackLevelCalibration(&hdcmipp, APP_CAMERA_CAPTURE_PIPE);
  if (status != HAL_OK)
  {
    return AppCamera_SetHalError(result, 21U, status);
  }

  exposure_conf.ShiftRed = 0U;
  exposure_conf.MultiplierRed = 128U;
  exposure_conf.ShiftGreen = 0U;
  exposure_conf.MultiplierGreen = 128U;
  exposure_conf.ShiftBlue = 0U;
  exposure_conf.MultiplierBlue = 128U;
  status = HAL_DCMIPP_PIPE_SetISPExposureConfig(&hdcmipp, APP_CAMERA_CAPTURE_PIPE, &exposure_conf);
  if (status != HAL_OK)
  {
    return AppCamera_SetHalError(result, 22U, status);
  }
  status = HAL_DCMIPP_PIPE_EnableISPExposure(&hdcmipp, APP_CAMERA_CAPTURE_PIPE);
  if (status != HAL_OK)
  {
    return AppCamera_SetHalError(result, 23U, status);
  }

  raw_bayer_conf.RawBayerType = DCMIPP_RAWBAYER_RGGB;
  raw_bayer_conf.PeakStrength = DCMIPP_RAWBAYER_ALGO_STRENGTH_8;
  raw_bayer_conf.EdgeStrength = DCMIPP_RAWBAYER_ALGO_STRENGTH_8;
  raw_bayer_conf.VLineStrength = DCMIPP_RAWBAYER_ALGO_STRENGTH_8;
  raw_bayer_conf.HLineStrength = DCMIPP_RAWBAYER_ALGO_STRENGTH_8;
  status = HAL_DCMIPP_PIPE_SetISPRawBayer2RGBConfig(&hdcmipp,
                                                    APP_CAMERA_CAPTURE_PIPE,
                                                    &raw_bayer_conf);
  if (status != HAL_OK)
  {
    return AppCamera_SetHalError(result, 24U, status);
  }
  status = HAL_DCMIPP_PIPE_EnableISPRawBayer2RGB(&hdcmipp, APP_CAMERA_CAPTURE_PIPE);
  if (status != HAL_OK)
  {
    return AppCamera_SetHalError(result, 25U, status);
  }

  status = HAL_DCMIPP_PIPE_EnableGammaConversion(&hdcmipp, APP_CAMERA_CAPTURE_PIPE);
  if (status != HAL_OK)
  {
    return AppCamera_SetHalError(result, 26U, status);
  }

  status = HAL_DCMIPP_PIPE_SetFrameCounterConfig(&hdcmipp, APP_CAMERA_CAPTURE_PIPE);
  if (status != HAL_OK)
  {
    return AppCamera_SetHalError(result, 27U, status);
  }
  status = HAL_DCMIPP_PIPE_ResetFrameCounter(&hdcmipp, APP_CAMERA_CAPTURE_PIPE);
  if (status != HAL_OK)
  {
    return AppCamera_SetHalError(result, 28U, status);
  }

  return HAL_OK;
}

static HAL_StatusTypeDef AppCamera_ConfigSensor(AppCamera_Result_t *result)
{
  uint32_t sensor_id = 0U;

  App_Print("CAMCFG sensor stage 30: read id\r\n");
  if (IMX219_ReadID(&AppCameraSensor, &sensor_id) != IMX219_OK)
  {
    return AppCamera_SetImxError(result, 30U);
  }
  if (sensor_id != IMX219_CHIP_ID)
  {
    if (result != NULL)
    {
      result->sensor_id = sensor_id;
    }
    return AppCamera_SetImxError(result, 31U);
  }

  AppCameraSensorId = sensor_id;
  if (result != NULL)
  {
    result->sensor_id = sensor_id;
  }

  App_Print("CAMCFG sensor stage 32: lp11\r\n");
  if (IMX219_EnterLp11(&AppCameraSensor) != IMX219_OK)
  {
    return AppCamera_SetImxError(result, 32U);
  }

  App_Print("CAMCFG sensor stage 33: init raw10\r\n");
  if (IMX219_Init(&AppCameraSensor, IMX219_R640_480_BIN4, IMX219_RAW10) != IMX219_OK)
  {
    return AppCamera_SetImxError(result, 33U);
  }

  App_Print("CAMCFG sensor stage 34: timing\r\n");
  if (IMX219_SetFrameTiming(&AppCameraSensor,
                            (uint16_t)APP_CAMERA_LINE_LENGTH,
                            (uint16_t)APP_CAMERA_FRAME_LENGTH) != IMX219_OK)
  {
    return AppCamera_SetImxError(result, 34U);
  }

  App_Print("CAMCFG sensor stage 35: low link\r\n");
  if (IMX219_SetDebugOpPllMultiplier(&AppCameraSensor,
                                     (uint16_t)APP_CAMERA_OP_PLL_MULT) != IMX219_OK)
  {
    return AppCamera_SetImxError(result, 35U);
  }

  App_Print("CAMCFG sensor stage 36: exposure\r\n");
  if (IMX219_SetExposureGain(&AppCameraSensor,
                             (uint16_t)APP_CAMERA_EXPOSURE_LINES,
                             (uint8_t)APP_CAMERA_ANALOG_GAIN,
                             (uint16_t)APP_CAMERA_DIGITAL_GAIN) != IMX219_OK)
  {
    return AppCamera_SetImxError(result, 36U);
  }

  App_Print("CAMCFG sensor stage 37: test pattern off\r\n");
  if (IMX219_SetTestPattern(&AppCameraSensor, 0U) != IMX219_OK)
  {
    return AppCamera_SetImxError(result, 37U);
  }

  if (result != NULL)
  {
    result->width = AppCameraSensor.Width;
    result->height = AppCameraSensor.Height;
  }

  return HAL_OK;
}

static void AppCamera_CleanFrameBuffer(void)
{
  SCB_CleanDCache_by_Addr((uint32_t *)AppMediaBuffer_GetCamera(),
                          (int32_t)APP_CAMERA_FRAME_BYTES);
  __DSB();
}

static void AppCamera_InvalidateFrameBuffer(void)
{
  SCB_InvalidateDCache_by_Addr((void *)AppMediaBuffer_GetCamera(),
                               (int32_t)APP_CAMERA_FRAME_BYTES);
  __DSB();
}

static void AppCamera_ResetCounters(void)
{
  AppCameraFrameCount = 0U;
  AppCameraVsyncCount = 0U;
  AppCameraSofCount = 0U;
  AppCameraEofCount = 0U;
  AppCameraPipeErrorCount = 0U;
  AppCameraCsiErrorCount = 0U;
  AppCameraLastError = 0U;
}

static void AppCamera_ReadSensorStatus(AppCamera_Status_t *status)
{
  uint16_t value;

  status->mode_reg = 0xFFFFU;
  status->line_reg = 0xFFFFU;
  status->frame_reg = 0xFFFFU;

  if (AppCameraBusRegistered == 0U)
  {
    return;
  }

  if (IMX219_ReadRegister16(&AppCameraSensor, 0x0100U, &value) == IMX219_OK)
  {
    status->mode_reg = value;
  }
  if (IMX219_ReadRegister16(&AppCameraSensor, 0x0162U, &value) == IMX219_OK)
  {
    status->line_reg = value;
  }
  if (IMX219_ReadRegister16(&AppCameraSensor, 0x0160U, &value) == IMX219_OK)
  {
    status->frame_reg = value;
  }
}

HAL_StatusTypeDef AppCamera_Probe(AppCamera_Result_t *result)
{
  uint32_t sensor_id = 0U;

  AppCamera_ClearResult(result);

  if (AppCamera_PowerAndRegister(result) != HAL_OK)
  {
    return HAL_ERROR;
  }

  if (IMX219_ReadID(&AppCameraSensor, &sensor_id) != IMX219_OK)
  {
    return AppCamera_SetImxError(result, 2U);
  }

  AppCameraSensorId = sensor_id;
  if (result != NULL)
  {
    result->sensor_id = sensor_id;
    result->width = AppCameraSensor.Width;
    result->height = AppCameraSensor.Height;
  }

  if (sensor_id != IMX219_CHIP_ID)
  {
    return AppCamera_SetImxError(result, 3U);
  }

  return HAL_OK;
}

HAL_StatusTypeDef AppCamera_Config(AppCamera_Result_t *result)
{
  HAL_StatusTypeDef status;

  AppCamera_ClearResult(result);
  status = AppCamera_CheckMediaBuffer(result);
  if (status != HAL_OK)
  {
    return status;
  }
  App_Print("CAMCFG start\r\n");

  if (AppCamera_PowerAndRegister(result) != HAL_OK)
  {
    return HAL_ERROR;
  }

  if (AppCameraStreaming != 0U)
  {
    (void)AppCamera_Stop(NULL);
  }

  status = AppCamera_ConfigDcmipp(result);
  if (status != HAL_OK)
  {
    App_Print("CAMCFG dcmipp failed\r\n");
    AppCameraDcmippConfigured = 0U;
    AppCameraConfigured = 0U;
    return status;
  }
  AppCameraDcmippConfigured = 1U;

  status = AppCamera_ConfigSensor(result);
  if (status != HAL_OK)
  {
    App_Print("CAMCFG sensor failed\r\n");
    AppCameraSensorConfigured = 0U;
    AppCameraConfigured = 0U;
    return status;
  }
  AppCameraSensorConfigured = 1U;

  memset(AppMediaBuffer_GetCamera(), 0xA5, APP_CAMERA_FRAME_BYTES);
  AppCameraConfigured = 1U;
  AppCameraStreaming = 0U;
  App_Print("CAMCFG done\r\n");

  return HAL_OK;
}

HAL_StatusTypeDef AppCamera_ConfigDcmippOnly(AppCamera_Result_t *result)
{
  HAL_StatusTypeDef status;

  AppCamera_ClearResult(result);
  App_Print("CAMCFGDCMIPP start\r\n");

  if (AppCameraStreaming != 0U)
  {
    (void)AppCamera_Stop(NULL);
  }

  status = AppCamera_ConfigDcmipp(result);
  if (status != HAL_OK)
  {
    App_Print("CAMCFGDCMIPP failed\r\n");
    AppCameraDcmippConfigured = 0U;
    AppCameraConfigured = 0U;
    return status;
  }

  AppCameraDcmippConfigured = 1U;
  AppCameraConfigured = (AppCameraSensorConfigured != 0U) ? 1U : 0U;
  if (result != NULL)
  {
    result->sensor_id = AppCameraSensorId;
    result->width = APP_CAMERA_WIDTH;
    result->height = APP_CAMERA_HEIGHT;
  }
  App_Print("CAMCFGDCMIPP done\r\n");

  return HAL_OK;
}

HAL_StatusTypeDef AppCamera_ConfigSensorOnly(AppCamera_Result_t *result)
{
  HAL_StatusTypeDef status;

  AppCamera_ClearResult(result);
  App_Print("CAMCFGSENSOR start\r\n");

  if (AppCamera_PowerAndRegister(result) != HAL_OK)
  {
    return HAL_ERROR;
  }

  if (AppCameraStreaming != 0U)
  {
    (void)AppCamera_Stop(NULL);
  }

  status = AppCamera_ConfigSensor(result);
  if (status != HAL_OK)
  {
    App_Print("CAMCFGSENSOR failed\r\n");
    AppCameraSensorConfigured = 0U;
    AppCameraConfigured = 0U;
    return status;
  }

  AppCameraSensorConfigured = 1U;
  AppCameraConfigured = (AppCameraDcmippConfigured != 0U) ? 1U : 0U;
  App_Print("CAMCFGSENSOR done\r\n");

  return HAL_OK;
}

HAL_StatusTypeDef AppCamera_Start(AppCamera_Result_t *result)
{
  HAL_StatusTypeDef status;

  AppCamera_ClearResult(result);
  status = AppCamera_CheckMediaBuffer(result);
  if (status != HAL_OK)
  {
    return status;
  }
  App_Print("CAMSTART start\r\n");

  if (AppCameraDcmippConfigured == 0U)
  {
    status = AppCamera_ConfigDcmippOnly(result);
    if (status != HAL_OK)
    {
      return status;
    }
  }

  if (AppCameraSensorConfigured == 0U)
  {
    status = AppCamera_ConfigSensorOnly(result);
    if (status != HAL_OK)
    {
      return status;
    }
  }

  if (AppCameraConfigured == 0U)
  {
    if ((AppCameraDcmippConfigured == 0U) || (AppCameraSensorConfigured == 0U))
    {
      return AppCamera_SetHalError(result, 39U, HAL_ERROR);
    }
    AppCameraConfigured = 1U;
  }

  if (AppCameraStreaming != 0U)
  {
    return HAL_OK;
  }

  AppCamera_ResetCounters();
  memset(AppMediaBuffer_GetCamera(), 0xA5, APP_CAMERA_FRAME_BYTES);
  AppCamera_CleanFrameBuffer();

  status = HAL_DCMIPP_PIPE_ResetFrameCounter(&hdcmipp, APP_CAMERA_CAPTURE_PIPE);
  if (status != HAL_OK)
  {
    return AppCamera_SetHalError(result, 40U, status);
  }

  status = HAL_DCMIPP_CSI_PIPE_Start(&hdcmipp,
                                     APP_CAMERA_CAPTURE_PIPE,
                                     APP_CAMERA_VIRTUAL_CHANNEL,
                                     (uint32_t)(uintptr_t)AppMediaBuffer_GetCamera(),
                                     APP_CAMERA_CAPTURE_MODE);
  if (status != HAL_OK)
  {
    return AppCamera_SetHalError(result, 41U, status);
  }

  App_DCMIPP_SetIrqEnabled(1U);
  if (IMX219_Start(&AppCameraSensor) != IMX219_OK)
  {
    App_DCMIPP_SetIrqEnabled(0U);
    (void)HAL_DCMIPP_CSI_PIPE_Stop(&hdcmipp,
                                   APP_CAMERA_CAPTURE_PIPE,
                                   APP_CAMERA_VIRTUAL_CHANNEL);
    return AppCamera_SetImxError(result, 42U);
  }

  AppCameraStreaming = 1U;
  App_Print("CAMSTART done\r\n");
  if (result != NULL)
  {
    result->sensor_id = AppCameraSensorId;
    result->width = AppCameraSensor.Width;
    result->height = AppCameraSensor.Height;
  }

  return HAL_OK;
}

HAL_StatusTypeDef AppCamera_Stop(AppCamera_Result_t *result)
{
  return AppCamera_FreezeLatestFrame(result);
}

HAL_StatusTypeDef AppCamera_FreezeLatestFrame(AppCamera_Result_t *result)
{
  HAL_StatusTypeDef status;

  AppCamera_ClearResult(result);
  if (AppCameraStreaming == 0U)
  {
    AppCamera_InvalidateFrameBuffer();
    return HAL_OK;
  }

  App_Print("CAMFREEZE start\r\n");
  App_DCMIPP_SetIrqEnabled(0U);
  status = HAL_DCMIPP_CSI_PIPE_Stop(&hdcmipp,
                                    APP_CAMERA_CAPTURE_PIPE,
                                    APP_CAMERA_VIRTUAL_CHANNEL);
  if (status != HAL_OK)
  {
    (void)IMX219_Stop(&AppCameraSensor);
    AppCameraStreaming = 0U;
    App_Print("CAMFREEZE pipe stop failed\r\n");
    return AppCamera_SetHalError(result, 60U, status);
  }

  if ((AppCameraBusRegistered != 0U) &&
      (IMX219_Stop(&AppCameraSensor) != IMX219_OK))
  {
    AppCameraStreaming = 0U;
    App_Print("CAMFREEZE sensor stop failed\r\n");
    return AppCamera_SetImxError(result, 61U);
  }

  HAL_Delay(20U);
  AppCamera_InvalidateFrameBuffer();
  AppCameraStreaming = 0U;
  if (result != NULL)
  {
    result->sensor_id = AppCameraSensorId;
    result->width = AppCameraSensor.Width;
    result->height = AppCameraSensor.Height;
  }
  App_Print("CAMFREEZE done\r\n");

  return HAL_OK;
}

void AppCamera_GetStatus(AppCamera_Status_t *status)
{
  if (status == NULL)
  {
    return;
  }

  memset(status, 0, sizeof(*status));
  status->initialized = AppCameraBusRegistered;
  status->configured = AppCameraConfigured;
  status->dcmipp_configured = AppCameraDcmippConfigured;
  status->sensor_configured = AppCameraSensorConfigured;
  status->streaming = AppCameraStreaming;
  status->sensor_id = AppCameraSensorId;
  status->width = AppCameraSensor.Width;
  status->height = AppCameraSensor.Height;
  status->frame_bytes = APP_CAMERA_FRAME_BYTES;
  status->buffer_addr = (uint32_t)(uintptr_t)AppMediaBuffer_GetCamera();
  status->frame_count = AppCameraFrameCount;
  status->vsync_count = AppCameraVsyncCount;
  status->sof_count = AppCameraSofCount;
  status->eof_count = AppCameraEofCount;
  status->pipe_error_count = AppCameraPipeErrorCount;
  status->csi_error_count = AppCameraCsiErrorCount;
  status->dcmipp_error = AppCameraLastError;
  status->p1sr = DCMIPP->P1SR;
  status->sr0 = CSI->SR0;
  status->sr1 = CSI->SR1;
  status->csi_err1 = CSI->ERR1;
  status->csi_err2 = CSI->ERR2;
  (void)HAL_DCMIPP_PIPE_ReadFrameCounter(&hdcmipp,
                                         APP_CAMERA_CAPTURE_PIPE,
                                         &status->hw_frame_count);
  AppCamera_ReadSensorStatus(status);
}

void AppCamera_GetFrameCounters(uint32_t *frame_count, uint32_t *hw_frame_count)
{
  if (frame_count != NULL)
  {
    *frame_count = AppCameraFrameCount;
  }
  if (hw_frame_count != NULL)
  {
    *hw_frame_count = 0U;
    (void)HAL_DCMIPP_PIPE_ReadFrameCounter(&hdcmipp,
                                           APP_CAMERA_CAPTURE_PIPE,
                                           hw_frame_count);
  }
}

const uint8_t *AppCamera_GetFrameBuffer(void)
{
  return AppMediaBuffer_GetCamera();
}

uint32_t AppCamera_GetFrameBufferSize(void)
{
  return AppMediaBuffer_GetCameraSize();
}

uint32_t AppCamera_GetFrameBytes(void)
{
  return APP_CAMERA_FRAME_BYTES;
}

void HAL_DCMIPP_PIPE_FrameEventCallback(DCMIPP_HandleTypeDef *hdcmipp_cb, uint32_t Pipe)
{
  if ((hdcmipp_cb == &hdcmipp) && (Pipe == APP_CAMERA_CAPTURE_PIPE))
  {
    AppCameraFrameCount++;
  }
}

void HAL_DCMIPP_PIPE_VsyncEventCallback(DCMIPP_HandleTypeDef *hdcmipp_cb, uint32_t Pipe)
{
  if ((hdcmipp_cb == &hdcmipp) && (Pipe == APP_CAMERA_CAPTURE_PIPE))
  {
    AppCameraVsyncCount++;
  }
}

void HAL_DCMIPP_PIPE_ErrorCallback(DCMIPP_HandleTypeDef *hdcmipp_cb, uint32_t Pipe)
{
  if ((hdcmipp_cb == &hdcmipp) && (Pipe == APP_CAMERA_CAPTURE_PIPE))
  {
    AppCameraPipeErrorCount++;
    AppCameraLastError = HAL_DCMIPP_GetError(hdcmipp_cb);
  }
}

void HAL_DCMIPP_ErrorCallback(DCMIPP_HandleTypeDef *hdcmipp_cb)
{
  if (hdcmipp_cb == &hdcmipp)
  {
    AppCameraPipeErrorCount++;
    AppCameraLastError = HAL_DCMIPP_GetError(hdcmipp_cb);
  }
}

void HAL_DCMIPP_CSI_StartOfFrameEventCallback(DCMIPP_HandleTypeDef *hdcmipp_cb,
                                              uint32_t VirtualChannel)
{
  if ((hdcmipp_cb == &hdcmipp) && (VirtualChannel == APP_CAMERA_VIRTUAL_CHANNEL))
  {
    AppCameraSofCount++;
  }
}

void HAL_DCMIPP_CSI_EndOfFrameEventCallback(DCMIPP_HandleTypeDef *hdcmipp_cb,
                                            uint32_t VirtualChannel)
{
  if ((hdcmipp_cb == &hdcmipp) && (VirtualChannel == APP_CAMERA_VIRTUAL_CHANNEL))
  {
    AppCameraEofCount++;
  }
}

void HAL_DCMIPP_CSI_LineErrorCallback(DCMIPP_HandleTypeDef *hdcmipp_cb, uint32_t DataLane)
{
  (void)DataLane;

  if (hdcmipp_cb == &hdcmipp)
  {
    AppCameraCsiErrorCount++;
    AppCameraLastError = HAL_DCMIPP_GetError(hdcmipp_cb);
  }
}
