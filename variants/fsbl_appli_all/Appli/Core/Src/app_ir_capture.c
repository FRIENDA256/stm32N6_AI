/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_ir_capture.c
  * @brief   Scheduled Tiny1C image and temperature acquisition.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "app_ir_capture.h"

#include "app_console.h"
#include "main.h"
#include "tiny1c_port_stm32_hal.h"

#include <string.h>

#define APP_IR_CAPTURE_THREAD_STACK_SIZE  4096U
#define APP_IR_CAPTURE_THREAD_PRIORITY    15U
#define APP_IR_CAPTURE_IMAGE_PERIOD_MS    (1000U / APP_IR_CAPTURE_IMAGE_FPS)
#define APP_IR_CAPTURE_TEMP_PERIOD_MS     (1000U / APP_IR_CAPTURE_TEMP_FPS)
#define APP_IR_CAPTURE_TEMP_PHASE_MS      (APP_IR_CAPTURE_IMAGE_PERIOD_MS / 2U)
typedef struct
{
  uint32_t sequence;
  uint32_t timestamp_ms;
  uint32_t length;
  uint8_t command;
} App_IRCapture_Slot_t;

static TX_THREAD AppIRCaptureThread;
static volatile App_IRCapture_Status_t AppIRCaptureStatus;
static App_IRCapture_Slot_t AppIRImageSlot = {.command = TINY1C_CMD_IMAGE};
static App_IRCapture_Slot_t AppIRTempSlot = {.command = TINY1C_CMD_TEMP};

static uint32_t AppIRCapture_Lock(void)
{
  uint32_t primask = __get_PRIMASK();

  __disable_irq();
  return primask;
}

static void AppIRCapture_Unlock(uint32_t primask)
{
  if (primask == 0U)
  {
    __enable_irq();
  }
}

static uint8_t AppIRCapture_TryBegin(void)
{
  uint32_t primask = AppIRCapture_Lock();

  if ((AppIRCaptureStatus.paused != 0U) || (AppIRCaptureStatus.active != 0U))
  {
    AppIRCapture_Unlock(primask);
    return 0U;
  }

  AppIRCaptureStatus.active = 1U;
  AppIRCapture_Unlock(primask);
  return 1U;
}

static void AppIRCapture_End(void)
{
  uint32_t primask = AppIRCapture_Lock();

  AppIRCaptureStatus.active = 0U;
  AppIRCapture_Unlock(primask);
}

static App_IRCapture_Slot_t *AppIRCapture_GetSlot(uint8_t command)
{
  if (command == TINY1C_CMD_IMAGE)
  {
    return &AppIRImageSlot;
  }
  if (command == TINY1C_CMD_TEMP)
  {
    return &AppIRTempSlot;
  }

  return NULL;
}

static void AppIRCapture_Publish(uint8_t command, uint32_t timestamp_ms)
{
  App_IRCapture_Slot_t *slot = AppIRCapture_GetSlot(command);
  const uint8_t *source;
  uint32_t length;
  uint8_t actual_command;

  if ((slot == NULL) ||
      (Tiny1C_STM32_GetLatestFrame(&source, &length, &actual_command, NULL) != TINY1C_STATUS_OK) ||
      (actual_command != command) ||
      (length > TINY1C_DEFAULT_FRAME_LEN))
  {
    return;
  }

  slot->sequence++;
  slot->timestamp_ms = timestamp_ms;
  slot->length = length;
}

static void AppIRCapture_Record(uint8_t command,
                                tiny1c_status_t capture_status,
                                uint32_t capture_ms,
                                uint32_t timestamp_ms,
                                uint8_t deadline_missed)
{
  uint32_t primask = AppIRCapture_Lock();

  AppIRCaptureStatus.last_command = command;
  AppIRCaptureStatus.last_capture_ms = capture_ms;
  if (capture_ms > AppIRCaptureStatus.max_capture_ms)
  {
    AppIRCaptureStatus.max_capture_ms = capture_ms;
  }
  if (deadline_missed != 0U)
  {
    AppIRCaptureStatus.deadline_miss_count++;
  }

  if (capture_status == TINY1C_STATUS_OK)
  {
    if (command == TINY1C_CMD_IMAGE)
    {
      AppIRCaptureStatus.image_count++;
      AppIRCaptureStatus.image_sequence = AppIRImageSlot.sequence;
      AppIRCaptureStatus.last_image_ms = timestamp_ms;
    }
    else
    {
      AppIRCaptureStatus.temp_count++;
      AppIRCaptureStatus.temp_sequence = AppIRTempSlot.sequence;
      AppIRCaptureStatus.last_temp_ms = timestamp_ms;
    }
  }
  else
  {
    AppIRCaptureStatus.capture_error_count++;
  }
  AppIRCapture_Unlock(primask);
}

static tiny1c_status_t AppIRCapture_Capture(uint8_t command, uint8_t deadline_missed)
{
  uint32_t start_ms;
  uint32_t end_ms;
  tiny1c_status_t status;

  if (AppIRCapture_TryBegin() == 0U)
  {
    return TINY1C_STATUS_UNSUPPORTED;
  }

  start_ms = HAL_GetTick();
  status = Tiny1C_STM32_CaptureFrameQuiet(command);
  end_ms = HAL_GetTick();
  if (status == TINY1C_STATUS_OK)
  {
    AppIRCapture_Publish(command, end_ms);
  }
  AppIRCapture_Record(command, status, end_ms - start_ms, end_ms, deadline_missed);
  AppIRCapture_End();
  return status;
}

static VOID AppIRCapture_ThreadEntry(ULONG thread_input)
{
  uint32_t next_image_ms;
  uint32_t next_temp_ms;
  uint32_t now_ms;
  tiny1c_status_t startup_status;

  (void)thread_input;
  App_Print("IR capture thread start target=image10fps,temp5fps\r\n");

  /* The first read starts preview, waits for sensor warm-up and drains stale frames. */
  do
  {
    startup_status = AppIRCapture_Capture(TINY1C_CMD_IMAGE, 0U);
    if (startup_status == TINY1C_STATUS_UNSUPPORTED)
    {
      tx_thread_sleep(1U);
    }
    else if (startup_status != TINY1C_STATUS_OK)
    {
      App_Print("IR capture startup retry\r\n");
      tx_thread_sleep(TX_TIMER_TICKS_PER_SECOND);
    }
  }
  while (startup_status != TINY1C_STATUS_OK);

  now_ms = HAL_GetTick();
  next_image_ms = now_ms + APP_IR_CAPTURE_IMAGE_PERIOD_MS;
  next_temp_ms = now_ms + APP_IR_CAPTURE_TEMP_PHASE_MS;

  {
    uint32_t primask = AppIRCapture_Lock();

    AppIRCaptureStatus.initialized = 1U;
    AppIRCaptureStatus.running = 1U;
    AppIRCaptureStatus.image_count = 0U;
    AppIRCaptureStatus.temp_count = 0U;
    AppIRCaptureStatus.capture_error_count = 0U;
    AppIRCaptureStatus.deadline_miss_count = 0U;
    AppIRCaptureStatus.last_capture_ms = 0U;
    AppIRCaptureStatus.max_capture_ms = 0U;
    AppIRCaptureStatus.schedule_elapsed_ms = now_ms;
    AppIRCapture_Unlock(primask);
  }
  App_Print("IR capture ready image=10fps temp=5fps\r\n");

  for (;;)
  {
    uint8_t command = 0U;
    uint8_t deadline_missed = 0U;
    uint32_t deadline_ms = 0U;
    uint32_t period_ms = 0U;

    now_ms = HAL_GetTick();
    if ((int32_t)(now_ms - next_image_ms) >= 0)
    {
      command = TINY1C_CMD_IMAGE;
      deadline_ms = next_image_ms;
      period_ms = APP_IR_CAPTURE_IMAGE_PERIOD_MS;
    }
    if (((int32_t)(now_ms - next_temp_ms) >= 0) &&
        ((command == 0U) || ((int32_t)(next_temp_ms - deadline_ms) < 0)))
    {
      command = TINY1C_CMD_TEMP;
      deadline_ms = next_temp_ms;
      period_ms = APP_IR_CAPTURE_TEMP_PERIOD_MS;
    }

    if (command == 0U)
    {
      tx_thread_sleep(1U);
      continue;
    }

    if ((uint32_t)(now_ms - deadline_ms) >= period_ms)
    {
      deadline_missed = 1U;
    }

    if (AppIRCapture_Capture(command, deadline_missed) == TINY1C_STATUS_UNSUPPORTED)
    {
      tx_thread_sleep(1U);
      continue;
    }

    now_ms = HAL_GetTick();
    if (command == TINY1C_CMD_IMAGE)
    {
      next_image_ms += APP_IR_CAPTURE_IMAGE_PERIOD_MS;
      if ((int32_t)(now_ms - next_image_ms) >= (int32_t)APP_IR_CAPTURE_IMAGE_PERIOD_MS)
      {
        next_image_ms = now_ms + APP_IR_CAPTURE_IMAGE_PERIOD_MS;
      }
    }
    else
    {
      next_temp_ms += APP_IR_CAPTURE_TEMP_PERIOD_MS;
      if ((int32_t)(now_ms - next_temp_ms) >= (int32_t)APP_IR_CAPTURE_TEMP_PERIOD_MS)
      {
        next_temp_ms = now_ms + APP_IR_CAPTURE_TEMP_PERIOD_MS;
      }
    }
  }
}

UINT App_IRCapture_Start(TX_BYTE_POOL *byte_pool)
{
  UCHAR *thread_stack;
  UINT status;

  if (byte_pool == TX_NULL)
  {
    return TX_PTR_ERROR;
  }

  (void)memset((void *)&AppIRCaptureStatus, 0, sizeof(AppIRCaptureStatus));
  status = tx_byte_allocate(byte_pool,
                            (VOID **)&thread_stack,
                            APP_IR_CAPTURE_THREAD_STACK_SIZE,
                            TX_NO_WAIT);
  if (status != TX_SUCCESS)
  {
    App_PrintHex32("IR capture byte allocate failed: ", status);
    return status;
  }
  (void)memset(thread_stack, 0, APP_IR_CAPTURE_THREAD_STACK_SIZE);

  status = tx_thread_create(&AppIRCaptureThread,
                            "Tiny1C capture",
                            AppIRCapture_ThreadEntry,
                            0,
                            thread_stack,
                            APP_IR_CAPTURE_THREAD_STACK_SIZE,
                            APP_IR_CAPTURE_THREAD_PRIORITY,
                            APP_IR_CAPTURE_THREAD_PRIORITY,
                            TX_NO_TIME_SLICE,
                            TX_AUTO_START);
  if (status != TX_SUCCESS)
  {
    App_PrintHex32("IR capture thread create failed: ", status);
    return status;
  }

  App_Print("IR capture thread created\r\n");
  return TX_SUCCESS;
}

UINT App_IRCapture_Pause(ULONG wait_ticks)
{
  ULONG start_tick = tx_time_get();
  uint32_t primask = AppIRCapture_Lock();

  AppIRCaptureStatus.paused = 1U;
  AppIRCapture_Unlock(primask);

  while (AppIRCaptureStatus.active != 0U)
  {
    if ((tx_time_get() - start_tick) >= wait_ticks)
    {
      App_IRCapture_Resume();
      return TX_NOT_DONE;
    }
    tx_thread_sleep(1U);
  }

  return TX_SUCCESS;
}

void App_IRCapture_Resume(void)
{
  uint32_t primask = AppIRCapture_Lock();

  AppIRCaptureStatus.paused = 0U;
  AppIRCapture_Unlock(primask);
}

void App_IRCapture_GetStatus(App_IRCapture_Status_t *status)
{
  uint32_t primask;

  if (status == NULL)
  {
    return;
  }

  primask = AppIRCapture_Lock();
  *status = AppIRCaptureStatus;
  AppIRCapture_Unlock(primask);
  if ((status->running != 0U) && (status->schedule_elapsed_ms != 0U))
  {
    status->schedule_elapsed_ms = HAL_GetTick() - status->schedule_elapsed_ms;
  }
  else
  {
    status->schedule_elapsed_ms = 0U;
  }
}
