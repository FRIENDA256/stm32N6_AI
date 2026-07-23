/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_stream_mode.c
  * @brief   Single-stream output mode selector (AD+AI vs IR).
  ******************************************************************************
  */
/* USER CODE END Header */

#include "app_stream_mode.h"

#include "app_ad7606.h"
#include "app_ai.h"
#include "app_ir_capture.h"
#include "app_ir_stream.h"

#include "tx_api.h"

#define APP_STREAM_MODE_SWITCH_TIMEOUT_TICKS  (2U * TX_TIMER_TICKS_PER_SECOND)

static volatile App_StreamMode_t AppStreamMode = APP_STREAM_MODE_SWITCHING;
static volatile uint32_t AppStreamModeActiveSenders;
static volatile uint8_t AppStreamModeInitialized;

static UINT AppStreamMode_Apply(App_StreamMode_t mode)
{
  UINT ad_status;
  UINT ai_status;
  UINT capture_status;
  UINT pause_status;
  UINT stream_status;

  if (mode == APP_STREAM_MODE_IR)
  {
    /* Stop bundle production and AD acquisition before enabling IR-only
       transmission. The AI thread drains and sleeps without new input. */
    App_AI_SetBundleConsumerActive(0U);
    ad_status = App_AD7606_SetActive(0U,
                                     APP_STREAM_MODE_SWITCH_TIMEOUT_TICKS);
    if (ad_status != TX_SUCCESS)
    {
      (void)App_AD7606_SetActive(1U, 0U);
      App_AI_SetBundleConsumerActive(1U);
      return ad_status;
    }
    ai_status = App_AI_SetProcessingActive(0U,
                                           APP_STREAM_MODE_SWITCH_TIMEOUT_TICKS);
    if (ai_status != TX_SUCCESS)
    {
      (void)App_AD7606_SetActive(1U, 0U);
      App_AI_SetBundleConsumerActive(1U);
      return ai_status;
    }
    capture_status = App_IRCapture_SetActive(1U);
    if (capture_status != TX_SUCCESS)
    {
      (void)App_AI_SetProcessingActive(1U, 0U);
      (void)App_AD7606_SetActive(1U, 0U);
      App_AI_SetBundleConsumerActive(1U);
      return capture_status;
    }
    App_IRCapture_Resume();

    stream_status = AppIRStream_SetActive(1U);
    if (stream_status != TX_SUCCESS)
    {
      (void)App_IRCapture_Pause(APP_STREAM_MODE_SWITCH_TIMEOUT_TICKS);
      (void)App_IRCapture_SetActive(0U);
      (void)App_AI_SetProcessingActive(1U, 0U);
      (void)App_AD7606_SetActive(1U, 0U);
      App_AI_SetBundleConsumerActive(1U);
    }
    return stream_status;
  }

  stream_status = AppIRStream_SetActive(0U);
  capture_status = App_IRCapture_SetActive(0U);
  pause_status = (capture_status == TX_SUCCESS) ?
                 App_IRCapture_Pause(APP_STREAM_MODE_SWITCH_TIMEOUT_TICKS) :
                 capture_status;
  ai_status = App_AI_SetProcessingActive(1U, 0U);
  App_AI_SetBundleConsumerActive(1U);
  ad_status = App_AD7606_SetActive(1U, 0U);
  if ((stream_status == TX_SUCCESS) &&
      (capture_status == TX_SUCCESS) &&
      (pause_status == TX_SUCCESS) &&
      (ai_status == TX_SUCCESS) &&
      (ad_status == TX_SUCCESS))
  {
    return TX_SUCCESS;
  }
  if (stream_status != TX_SUCCESS)
  {
    return stream_status;
  }
  if (capture_status != TX_SUCCESS)
  {
    return capture_status;
  }
  if (pause_status != TX_SUCCESS)
  {
    return pause_status;
  }
  return (ai_status != TX_SUCCESS) ? ai_status : ad_status;
}

static void AppStreamMode_Publish(App_StreamMode_t mode)
{
  TX_INTERRUPT_SAVE_AREA

  TX_DISABLE
  AppStreamMode = mode;
  TX_RESTORE
}

static uint32_t AppStreamMode_GetActiveSenders(void)
{
  uint32_t active_senders;
  TX_INTERRUPT_SAVE_AREA

  TX_DISABLE
  active_senders = AppStreamModeActiveSenders;
  TX_RESTORE
  return active_senders;
}

UINT App_StreamMode_Init(void)
{
  UINT ad_status;
  UINT ai_status;
  UINT capture_status;
  UINT pause_status;
  UINT stream_status;
  TX_INTERRUPT_SAVE_AREA

  TX_DISABLE
  AppStreamMode = APP_STREAM_MODE_SWITCHING;
  AppStreamModeActiveSenders = 0U;
  AppStreamModeInitialized = 0U;
  TX_RESTORE

  capture_status = App_IRCapture_SetActive(0U);
  pause_status = (capture_status == TX_SUCCESS) ?
                 App_IRCapture_Pause(APP_STREAM_MODE_SWITCH_TIMEOUT_TICKS) :
                 capture_status;
  stream_status = AppIRStream_SetActive(0U);
  ai_status = App_AI_SetProcessingActive(1U, 0U);
  App_AI_SetBundleConsumerActive(1U);
  ad_status = App_AD7606_SetActive(1U, 0U);

  TX_DISABLE
  AppStreamMode = APP_STREAM_MODE_ADAI;
  AppStreamModeInitialized = 1U;
  TX_RESTORE
  if (capture_status != TX_SUCCESS)
  {
    return capture_status;
  }
  if (pause_status != TX_SUCCESS)
  {
    return pause_status;
  }
  if (stream_status != TX_SUCCESS)
  {
    return stream_status;
  }
  return (ai_status != TX_SUCCESS) ? ai_status : ad_status;
}

App_StreamMode_t App_StreamMode_Get(void)
{
  return AppStreamMode;
}

UINT App_StreamMode_Set(App_StreamMode_t mode)
{
  App_StreamMode_t previous;
  ULONG start_tick;
  UINT status;
  TX_INTERRUPT_SAVE_AREA

  if ((mode != APP_STREAM_MODE_ADAI) && (mode != APP_STREAM_MODE_IR))
  {
    return TX_OPTION_ERROR;
  }

  TX_DISABLE
  if (AppStreamModeInitialized == 0U)
  {
    TX_RESTORE
    return TX_NOT_DONE;
  }
  previous = AppStreamMode;
  if (previous == mode)
  {
    TX_RESTORE
    return TX_SUCCESS;
  }
  if (previous == APP_STREAM_MODE_SWITCHING)
  {
    TX_RESTORE
    return TX_NOT_DONE;
  }
  AppStreamMode = APP_STREAM_MODE_SWITCHING;
  TX_RESTORE

  /* Stop bundle production immediately while an in-flight AD+AI message
     drains. This keeps the mode transition from filling the queue. */
  if (mode == APP_STREAM_MODE_IR)
  {
    App_AI_SetBundleConsumerActive(0U);
  }
  else
  {
    /* Release IR capture CPU time before waiting for the last IR message. */
    status = App_IRCapture_SetActive(0U);
    if (status != TX_SUCCESS)
    {
      AppStreamMode_Publish(previous);
      return status;
    }
    status = App_IRCapture_Pause(APP_STREAM_MODE_SWITCH_TIMEOUT_TICKS);
    if (status != TX_SUCCESS)
    {
      (void)App_IRCapture_SetActive(1U);
      AppStreamMode_Publish(previous);
      return status;
    }
  }

  start_tick = tx_time_get();
  while (AppStreamMode_GetActiveSenders() != 0U)
  {
    if ((tx_time_get() - start_tick) >= APP_STREAM_MODE_SWITCH_TIMEOUT_TICKS)
    {
      if (previous == APP_STREAM_MODE_ADAI)
      {
        App_AI_SetBundleConsumerActive(1U);
      }
      else
      {
        (void)App_IRCapture_SetActive(1U);
        App_IRCapture_Resume();
      }
      AppStreamMode_Publish(previous);
      return TX_NOT_DONE;
    }
    tx_thread_sleep(1U);
  }

  status = AppStreamMode_Apply(mode);
  if (status != TX_SUCCESS)
  {
    (void)AppStreamMode_Apply(previous);
    AppStreamMode_Publish(previous);
    return status;
  }

  AppStreamMode_Publish(mode);
  return TX_SUCCESS;
}

uint8_t App_StreamMode_TryAcquireSend(App_StreamMode_t mode)
{
  uint8_t acquired = 0U;
  TX_INTERRUPT_SAVE_AREA

  TX_DISABLE
  if ((AppStreamModeInitialized != 0U) && (AppStreamMode == mode))
  {
    AppStreamModeActiveSenders++;
    acquired = 1U;
  }
  TX_RESTORE
  return acquired;
}

void App_StreamMode_ReleaseSend(void)
{
  TX_INTERRUPT_SAVE_AREA

  TX_DISABLE
  if (AppStreamModeActiveSenders != 0U)
  {
    AppStreamModeActiveSenders--;
  }
  TX_RESTORE
}
