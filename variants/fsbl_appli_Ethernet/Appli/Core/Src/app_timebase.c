/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_timebase.c
  * @brief   HAL/ThreadX time-base helpers.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "app_timebase.h"
#include "main.h"
#include "tx_api.h"

extern volatile ULONG _tx_thread_system_state;

void App_PreThreadXTickStop(void)
{
  SysTick->CTRL = 0U;
  SysTick->VAL = 0U;
  SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;
}

uint32_t HAL_GetTick(void)
{
  static uint32_t tx_tick_offset_ms;
  static uint32_t tx_tick_started;
  ULONG tx_ticks;
  uint32_t tx_tick_ms;

  if (_tx_thread_system_state != 0U)
  {
    return uwTick;
  }

  tx_ticks = tx_time_get();

#if (TX_TIMER_TICKS_PER_SECOND == 1000U)
  tx_tick_ms = (uint32_t)tx_ticks;
#else
  tx_tick_ms = (uint32_t)(((tx_ticks / TX_TIMER_TICKS_PER_SECOND) * 1000U) +
                          (((tx_ticks % TX_TIMER_TICKS_PER_SECOND) * 1000U) /
                           TX_TIMER_TICKS_PER_SECOND));
#endif

  if ((tx_tick_started == 0U) && (tx_ticks != 0U))
  {
    tx_tick_offset_ms = uwTick;
    tx_tick_started = 1U;
  }

  if (tx_tick_started != 0U)
  {
    return tx_tick_offset_ms + tx_tick_ms;
  }

  return uwTick;
}
