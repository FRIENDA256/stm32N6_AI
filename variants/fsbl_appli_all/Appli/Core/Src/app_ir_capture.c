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
#include "app_media_buffer.h"
#include "main.h"
#include "tiny1c_port_stm32_hal.h"

#include <stdio.h>
#include <string.h>

#define APP_IR_CAPTURE_THREAD_STACK_SIZE  4096U
/* Match the proven TCP-command capture priority. The thread blocks on every
   DMA chunk, so NetX core/echo threads still preempt it while AD7606 can run
   between HPDMA completions. */
/* Single-stream mode gates which stream sends, so IR capture priority is now
 * switched at runtime by app_stream_mode:
 *   IDLE (13): AD+AI mode. The capture task is paused after one startup
 *              warm-up frame and performs no periodic SPI transfers.
 *   ACTIVE (13): IR mode. The current 16 KB SPI chunk configuration already
 *                sustains the target rate; keeping capture below AD/AI and
 *                the control path prevents the IR sender from being starved.
 * The thread is created at IDLE (default power-on = AD+AI mode). */
#define APP_IR_CAPTURE_PRIORITY_IDLE      13U
#define APP_IR_CAPTURE_PRIORITY_ACTIVE    13U
#define APP_IR_CAPTURE_THREAD_PRIORITY    APP_IR_CAPTURE_PRIORITY_IDLE
#define APP_IR_CAPTURE_EXT_RAM_BASE       0x90000000UL
#define APP_IR_CAPTURE_EXT_RAM_SIZE       0x02000000UL
#define APP_IR_CAPTURE_FRAME_SLOTS        3U
#define APP_IR_CAPTURE_INVALID_SLOT       0xFFFFFFFFUL
#define APP_IR_CAPTURE_PSRAM_STATUS_ADDR  0x341FE000UL
#define APP_IR_CAPTURE_PSRAM_MAGIC        0x5053524DUL
#define APP_IR_CAPTURE_PSRAM_VERSION      1U
#define APP_IR_CAPTURE_PSRAM_READY        1U

#if (APP_IR_CAPTURE_FRAME_SLOTS > APP_MEDIA_TINY1C_FRAME_SLOTS)
#error "Tiny1C capture slot count exceeds the shared media buffer"
#endif
#if (TINY1C_DEFAULT_FRAME_LEN > APP_MEDIA_TINY1C_FRAME_BYTES)
#error "Tiny1C frame does not fit in a shared media buffer slot"
#endif

#if (APP_IR_CAPTURE_IMAGE_FPS > 0U)
#define APP_IR_CAPTURE_IMAGE_PERIOD_MS    (1000U / APP_IR_CAPTURE_IMAGE_FPS)
#endif
#if (APP_IR_CAPTURE_TEMP_FPS > 0U)
#define APP_IR_CAPTURE_TEMP_PERIOD_MS     (1000U / APP_IR_CAPTURE_TEMP_FPS)
#endif
#if (APP_IR_CAPTURE_SNAPSHOT_FPS > 0U)
#define APP_IR_CAPTURE_SNAPSHOT_PERIOD_MS \
  (1000U / APP_IR_CAPTURE_SNAPSHOT_FPS)
#endif
#if ((APP_IR_CAPTURE_IMAGE_FPS > 0U) && (APP_IR_CAPTURE_TEMP_FPS > 0U))
#define APP_IR_CAPTURE_TEMP_PHASE_MS      (APP_IR_CAPTURE_IMAGE_PERIOD_MS / 2U)
#elif (APP_IR_CAPTURE_TEMP_FPS > 0U)
#define APP_IR_CAPTURE_TEMP_PHASE_MS      APP_IR_CAPTURE_TEMP_PERIOD_MS
#endif

#if ((APP_IR_CAPTURE_IMAGE_FPS == 0U) && (APP_IR_CAPTURE_TEMP_FPS == 0U))
#error "At least one Tiny1C capture stream must be enabled"
#endif

#if (APP_IR_CAPTURE_TEMP_FPS > 0U)
#define APP_IR_CAPTURE_STARTUP_COMMAND TINY1C_CMD_TEMP
#else
#define APP_IR_CAPTURE_STARTUP_COMMAND TINY1C_CMD_IMAGE
#endif

typedef struct
{
  uint32_t sequence;
  uint32_t timestamp_ms;
  uint32_t length;
  uint8_t command;
} App_IRCapture_Slot_t;

typedef struct
{
  uint32_t magic;
  uint32_t version;
  uint32_t status;
  uint32_t step;
  uint32_t error_code;
  uint32_t hal_error;
  uint32_t base_address;
  uint32_t size_bytes;
  uint32_t test_offset;
  uint32_t test_expected;
  uint32_t test_actual;
} App_IRCapture_PsramStatus_t;

typedef struct
{
  uint32_t sequence;
  uint32_t timestamp_ms;
  uint32_t length;
  uint32_t crc32;
  uint32_t readers;
  uint8_t command;
  uint8_t valid;
  uint8_t writing;
} App_IRCapture_FrameSlot_t;

static TX_THREAD AppIRCaptureThread;
static volatile App_IRCapture_Status_t AppIRCaptureStatus;
static App_IRCapture_Slot_t AppIRImageSlot = {.command = TINY1C_CMD_IMAGE};
static App_IRCapture_Slot_t AppIRTempSlot = {.command = TINY1C_CMD_TEMP};
static volatile App_IRCapture_FrameSlot_t
  AppIRFrameSlots[APP_IR_CAPTURE_FRAME_SLOTS];
static volatile uint32_t AppIRPublishedFrameSlot = APP_IR_CAPTURE_INVALID_SLOT;
static volatile uint8_t AppIRCaptureHighRate;

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

static uint8_t AppIRCapture_TryBegin(uint8_t allow_when_paused)
{
  uint32_t primask = AppIRCapture_Lock();

  if (((allow_when_paused == 0U) &&
       (AppIRCaptureStatus.paused != 0U)) ||
      (AppIRCaptureStatus.active != 0U))
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

static uint8_t AppIRCapture_ExternalRamReady(void)
{
  const volatile App_IRCapture_PsramStatus_t *status =
    (const volatile App_IRCapture_PsramStatus_t *)APP_IR_CAPTURE_PSRAM_STATUS_ADDR;

  return ((status->magic == APP_IR_CAPTURE_PSRAM_MAGIC) &&
          (status->version == APP_IR_CAPTURE_PSRAM_VERSION) &&
          (status->status == APP_IR_CAPTURE_PSRAM_READY) &&
          (status->base_address == APP_IR_CAPTURE_EXT_RAM_BASE) &&
          (status->size_bytes >= APP_IR_CAPTURE_EXT_RAM_SIZE)) ? 1U : 0U;
}

static void AppIRCapture_ReadExternalRamStatus(App_IRCapture_Status_t *status)
{
  const volatile App_IRCapture_PsramStatus_t *psram_status =
    (const volatile App_IRCapture_PsramStatus_t *)APP_IR_CAPTURE_PSRAM_STATUS_ADDR;

  if (status == NULL)
  {
    return;
  }

  status->external_ram_magic = psram_status->magic;
  status->external_ram_status = psram_status->status;
  status->external_ram_step = psram_status->step;
  status->external_ram_error = psram_status->error_code;
  status->external_ram_hal_error = psram_status->hal_error;
}

static uint8_t *AppIRCapture_GetFrameSlotAddress(uint32_t slot_index)
{
  return AppMediaBuffer_GetTiny1CSlot(slot_index);
}

static uint32_t AppIRCapture_ReserveFrameSlot(void)
{
  uint32_t primask = AppIRCapture_Lock();
  uint32_t slot_index = APP_IR_CAPTURE_INVALID_SLOT;

  for (uint32_t index = 0U; index < APP_IR_CAPTURE_FRAME_SLOTS; index++)
  {
    if ((index != AppIRPublishedFrameSlot) &&
        (AppIRFrameSlots[index].readers == 0U) &&
        (AppIRFrameSlots[index].writing == 0U))
    {
      AppIRFrameSlots[index].writing = 1U;
      slot_index = index;
      break;
    }
  }
  AppIRCapture_Unlock(primask);
  return slot_index;
}

static void AppIRCapture_CancelFrameSlot(uint32_t slot_index)
{
  uint32_t primask;

  if (slot_index >= APP_IR_CAPTURE_FRAME_SLOTS)
  {
    return;
  }

  primask = AppIRCapture_Lock();
  AppIRFrameSlots[slot_index].writing = 0U;
  AppIRCapture_Unlock(primask);
}

static void AppIRCapture_PublishFrameSlot(uint32_t slot_index,
                                          uint8_t command,
                                          uint32_t sequence,
                                          uint32_t timestamp_ms,
                                          uint32_t length,
                                          uint32_t crc32)
{
  uint32_t primask = AppIRCapture_Lock();

  AppIRFrameSlots[slot_index].sequence = sequence;
  AppIRFrameSlots[slot_index].timestamp_ms = timestamp_ms;
  AppIRFrameSlots[slot_index].length = length;
  AppIRFrameSlots[slot_index].crc32 = crc32;
  AppIRFrameSlots[slot_index].command = command;
  AppIRFrameSlots[slot_index].valid = 1U;
  AppIRFrameSlots[slot_index].writing = 0U;
  AppIRPublishedFrameSlot = slot_index;
  AppIRCaptureStatus.snapshot_publish_count++;
  AppIRCapture_Unlock(primask);
}

static void AppIRCapture_Publish(uint8_t command,
                                 uint32_t timestamp_ms,
                                 uint32_t frame_slot)
{
  App_IRCapture_Slot_t *slot = AppIRCapture_GetSlot(command);
  uint32_t sequence;

  if ((slot == NULL) || (frame_slot >= APP_IR_CAPTURE_FRAME_SLOTS))
  {
    return;
  }

  sequence = slot->sequence + 1U;
  slot->sequence = sequence;
  slot->timestamp_ms = timestamp_ms;
  slot->length = TINY1C_DEFAULT_FRAME_LEN;

  AppIRCapture_PublishFrameSlot(frame_slot,
                                command,
                                sequence,
                                timestamp_ms,
                                TINY1C_DEFAULT_FRAME_LEN,
                                0U);
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
  AppIRCaptureStatus.capture_total_ms += capture_ms;
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
      AppIRCaptureStatus.image_capture_total_ms += capture_ms;
      AppIRCaptureStatus.image_sequence = AppIRImageSlot.sequence;
      AppIRCaptureStatus.last_image_ms = timestamp_ms;
    }
    else
    {
      AppIRCaptureStatus.temp_count++;
      AppIRCaptureStatus.temp_capture_total_ms += capture_ms;
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

static tiny1c_status_t AppIRCapture_Capture(uint8_t command,
                                            uint8_t deadline_missed,
                                            uint8_t allow_when_paused)
{
  const uint8_t *captured_frame;
  uint32_t captured_length;
  uint8_t captured_command;
  uint8_t *frame_buffer;
  uint32_t frame_slot;
  uint32_t start_ms;
  uint32_t end_ms;
  tiny1c_status_t status;

  if (AppIRCapture_TryBegin(allow_when_paused) == 0U)
  {
    return TINY1C_STATUS_UNSUPPORTED;
  }

  frame_slot = AppIRCapture_ReserveFrameSlot();
  if (frame_slot == APP_IR_CAPTURE_INVALID_SLOT)
  {
    uint32_t primask = AppIRCapture_Lock();

    AppIRCaptureStatus.snapshot_publish_miss_count++;
    AppIRCapture_Unlock(primask);
    AppIRCapture_End();
    return TINY1C_STATUS_UNSUPPORTED;
  }

  start_ms = HAL_GetTick();
  frame_buffer = AppIRCapture_GetFrameSlotAddress(frame_slot);
  status = Tiny1C_STM32_CaptureFrameQuietInto(command,
                                              frame_buffer,
                                              APP_MEDIA_TINY1C_FRAME_BYTES);
  end_ms = HAL_GetTick();
  if (status == TINY1C_STATUS_OK)
  {
    status = Tiny1C_STM32_GetLatestFrame(&captured_frame,
                                         &captured_length,
                                         &captured_command,
                                         NULL);
    if ((status == TINY1C_STATUS_OK) &&
        (captured_frame == frame_buffer) &&
        (captured_command == command) &&
        (captured_length <= TINY1C_DEFAULT_FRAME_LEN))
    {
      AppIRCapture_Publish(command, end_ms, frame_slot);
    }
    else
    {
      status = TINY1C_STATUS_ERROR;
      AppIRCapture_CancelFrameSlot(frame_slot);
    }
  }
  else
  {
    AppIRCapture_CancelFrameSlot(frame_slot);
  }
  AppIRCapture_Record(command, status, end_ms - start_ms, end_ms, deadline_missed);
  AppIRCapture_End();
  return status;
}

static VOID AppIRCapture_ThreadEntry(ULONG thread_input)
{
  char config_line[112];
#if (APP_IR_CAPTURE_IMAGE_FPS > 0U)
  uint32_t next_image_ms;
#endif
#if (APP_IR_CAPTURE_TEMP_FPS > 0U)
  uint32_t next_temp_ms;
#endif
  uint32_t now_ms;
  uint8_t scheduled_high_rate;
  tiny1c_status_t startup_status;

  (void)thread_input;
  (void)snprintf(config_line,
                 sizeof(config_line),
                 "IR capture thread start target=image%lufps,temp%lufps spi=%luHz\r\n",
                 (unsigned long)APP_IR_CAPTURE_IMAGE_FPS,
                 (unsigned long)APP_IR_CAPTURE_TEMP_FPS,
                 (unsigned long)Tiny1C_STM32_GetSpiClockHz());
  App_Print(config_line);

  /* The first read starts preview, waits for sensor warm-up and drains stale frames. */
  do
  {
    startup_status = AppIRCapture_Capture(APP_IR_CAPTURE_STARTUP_COMMAND,
                                           0U,
                                           1U);
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
#if (APP_IR_CAPTURE_IMAGE_FPS > 0U)
  next_image_ms = now_ms + APP_IR_CAPTURE_IMAGE_PERIOD_MS;
#endif
#if (APP_IR_CAPTURE_TEMP_FPS > 0U)
  next_temp_ms = now_ms + APP_IR_CAPTURE_TEMP_PHASE_MS;
#endif
  scheduled_high_rate = AppIRCaptureHighRate;

  {
    uint32_t primask = AppIRCapture_Lock();

    AppIRCaptureStatus.initialized = 1U;
    AppIRCaptureStatus.running = 1U;
    AppIRCaptureStatus.image_count = 0U;
    AppIRCaptureStatus.temp_count = 0U;
    AppIRCaptureStatus.image_capture_total_ms = 0U;
    AppIRCaptureStatus.temp_capture_total_ms = 0U;
    AppIRCaptureStatus.capture_total_ms = 0U;
    AppIRCaptureStatus.capture_error_count = 0U;
    AppIRCaptureStatus.deadline_miss_count = 0U;
    AppIRCaptureStatus.last_capture_ms = 0U;
    AppIRCaptureStatus.max_capture_ms = 0U;
    AppIRCaptureStatus.schedule_elapsed_ms = now_ms;
    AppIRCapture_Unlock(primask);
  }
  (void)snprintf(config_line,
                 sizeof(config_line),
                 "IR capture ready image=%lufps temp=%lufps spi=%luHz\r\n",
                 (unsigned long)APP_IR_CAPTURE_IMAGE_FPS,
                 (unsigned long)APP_IR_CAPTURE_TEMP_FPS,
                 (unsigned long)Tiny1C_STM32_GetSpiClockHz());
  App_Print(config_line);

  for (;;)
  {
    uint8_t command = 0U;
    uint8_t deadline_missed = 0U;
    uint32_t deadline_ms = 0U;
    uint32_t period_ms = 0U;
    uint32_t temp_period_ms =
      (AppIRCaptureHighRate != 0U) ?
      (1000U / APP_IR_CAPTURE_TEMP_FPS) :
      (1000U / APP_IR_CAPTURE_IDLE_TEMP_FPS);

    now_ms = HAL_GetTick();
    if (AppIRCaptureStatus.paused != 0U)
    {
      tx_thread_sleep(5U);
      continue;
    }
#if (APP_IR_CAPTURE_TEMP_FPS > 0U)
    if (scheduled_high_rate != AppIRCaptureHighRate)
    {
      scheduled_high_rate = AppIRCaptureHighRate;
      next_temp_ms = now_ms;
    }
#endif
#if (APP_IR_CAPTURE_IMAGE_FPS > 0U)
    if ((int32_t)(now_ms - next_image_ms) >= 0)
    {
      command = TINY1C_CMD_IMAGE;
      deadline_ms = next_image_ms;
      period_ms = APP_IR_CAPTURE_IMAGE_PERIOD_MS;
    }
#endif
#if (APP_IR_CAPTURE_TEMP_FPS > 0U)
    if (((int32_t)(now_ms - next_temp_ms) >= 0) &&
        ((command == 0U) || ((int32_t)(next_temp_ms - deadline_ms) < 0)))
    {
      command = TINY1C_CMD_TEMP;
      deadline_ms = next_temp_ms;
      period_ms = temp_period_ms;
    }
#endif

    if (command == 0U)
    {
      tx_thread_sleep(1U);
      continue;
    }

    if ((uint32_t)(now_ms - deadline_ms) >= period_ms)
    {
      deadline_missed = 1U;
    }

    if (AppIRCapture_Capture(command, deadline_missed, 0U) == TINY1C_STATUS_UNSUPPORTED)
    {
      tx_thread_sleep(1U);
      continue;
    }

    now_ms = HAL_GetTick();
#if (APP_IR_CAPTURE_IMAGE_FPS > 0U)
    if (command == TINY1C_CMD_IMAGE)
    {
      next_image_ms += APP_IR_CAPTURE_IMAGE_PERIOD_MS;
      if ((int32_t)(now_ms - next_image_ms) >= (int32_t)APP_IR_CAPTURE_IMAGE_PERIOD_MS)
      {
        next_image_ms = now_ms + APP_IR_CAPTURE_IMAGE_PERIOD_MS;
      }
    }
#endif
#if (APP_IR_CAPTURE_TEMP_FPS > 0U)
    if (command == TINY1C_CMD_TEMP)
    {
      next_temp_ms += temp_period_ms;
      if ((int32_t)(now_ms - next_temp_ms) >= (int32_t)temp_period_ms)
      {
        next_temp_ms = now_ms + temp_period_ms;
      }
    }
#endif
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
  (void)memset((void *)AppIRFrameSlots, 0, sizeof(AppIRFrameSlots));
  AppIRPublishedFrameSlot = APP_IR_CAPTURE_INVALID_SLOT;
  AppIRCaptureHighRate = 0U;
  AppIRCaptureStatus.external_ram_ready = AppIRCapture_ExternalRamReady();
  AppIRCapture_ReadExternalRamStatus((App_IRCapture_Status_t *)&AppIRCaptureStatus);
  if (AppIRCaptureStatus.external_ram_ready != 0U)
  {
    App_Print("IR external RAM ready for future media buffering\r\n");
  }
  else
  {
    App_Print("IR external RAM unavailable; internal frame pool active\r\n");
  }
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

UINT App_IRCapture_SetActive(uint8_t active)
{
  UINT new_priority =
    (active != 0U) ? APP_IR_CAPTURE_PRIORITY_ACTIVE : APP_IR_CAPTURE_PRIORITY_IDLE;
  UINT old_priority;
  UINT status;

  status = tx_thread_priority_change(&AppIRCaptureThread,
                                     new_priority,
                                     &old_priority);
  if (status == TX_SUCCESS)
  {
    uint32_t primask = AppIRCapture_Lock();
    if ((active != 0U) && (AppIRCaptureHighRate == 0U))
    {
      AppIRPublishedFrameSlot = APP_IR_CAPTURE_INVALID_SLOT;
    }
    AppIRCaptureHighRate = (active != 0U) ? 1U : 0U;
    AppIRCapture_Unlock(primask);
  }
  return status;
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
  status->target_image_fps = APP_IR_CAPTURE_IMAGE_FPS;
  status->target_temp_fps = (AppIRCaptureHighRate != 0U) ?
                            APP_IR_CAPTURE_TEMP_FPS :
                            0U;
  status->spi_clock_hz = Tiny1C_STM32_GetSpiClockHz();
  AppIRCapture_ReadExternalRamStatus(status);
  if ((status->running != 0U) && (status->schedule_elapsed_ms != 0U))
  {
    status->schedule_elapsed_ms = HAL_GetTick() - status->schedule_elapsed_ms;
  }
  else
  {
    status->schedule_elapsed_ms = 0U;
  }
}

UINT App_IRCapture_AcquireLatestFrame(uint8_t command,
                                      App_IRCapture_FrameLease_t *lease)
{
  uint32_t primask;
  uint32_t slot_index;

  if (lease == NULL)
  {
    return TX_PTR_ERROR;
  }
  (void)memset(lease, 0, sizeof(*lease));
  lease->slot_index = APP_IR_CAPTURE_INVALID_SLOT;

  primask = AppIRCapture_Lock();
  slot_index = AppIRPublishedFrameSlot;
  if ((slot_index >= APP_IR_CAPTURE_FRAME_SLOTS) ||
      (AppIRFrameSlots[slot_index].valid == 0U) ||
      (AppIRFrameSlots[slot_index].writing != 0U) ||
      (AppIRFrameSlots[slot_index].command != command))
  {
    AppIRCapture_Unlock(primask);
    return TX_NOT_AVAILABLE;
  }

  AppIRFrameSlots[slot_index].readers++;
  lease->data = AppIRCapture_GetFrameSlotAddress(slot_index);
  lease->length = AppIRFrameSlots[slot_index].length;
  lease->sequence = AppIRFrameSlots[slot_index].sequence;
  lease->timestamp_ms = AppIRFrameSlots[slot_index].timestamp_ms;
  lease->crc32 = AppIRFrameSlots[slot_index].crc32;
  lease->slot_index = slot_index;
  lease->command = AppIRFrameSlots[slot_index].command;
  AppIRCapture_Unlock(primask);

  return TX_SUCCESS;
}

void App_IRCapture_ReleaseFrame(const App_IRCapture_FrameLease_t *lease)
{
  uint32_t primask;

  if ((lease == NULL) || (lease->slot_index >= APP_IR_CAPTURE_FRAME_SLOTS))
  {
    return;
  }

  primask = AppIRCapture_Lock();
  if (AppIRFrameSlots[lease->slot_index].readers > 0U)
  {
    AppIRFrameSlots[lease->slot_index].readers--;
  }
  AppIRCapture_Unlock(primask);
}
