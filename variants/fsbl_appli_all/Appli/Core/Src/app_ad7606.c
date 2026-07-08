/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_ad7606.c
  * @brief   ThreadX wrapper for the AD7606 SPI4 DMA acquisition path.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "app_ad7606.h"

#include "ad7606_spi_dma.h"
#include "app_console.h"

#include <string.h>

#define APP_AD7606_THREAD_STACK_SIZE  4096U
#define APP_AD7606_THREAD_PRIORITY    14U
#define APP_AD7606_THREAD_SLEEP_TICKS 1U

static TX_THREAD AppAd7606Thread;

static UINT AppAD7606_ByteAllocate(TX_BYTE_POOL *byte_pool, UCHAR **memory, ULONG size)
{
  UINT status;

  if ((byte_pool == TX_NULL) || (memory == TX_NULL))
  {
    return TX_PTR_ERROR;
  }

  status = tx_byte_allocate(byte_pool, (VOID **)memory, size, TX_NO_WAIT);
  if (status != TX_SUCCESS)
  {
    App_PrintHex32("AD7606 byte allocate failed: ", status);
    return status;
  }

  (void)memset(*memory, 0, size);
  return TX_SUCCESS;
}

static VOID AppAD7606_ThreadEntry(ULONG thread_input)
{
  (void)thread_input;

  App_Print("AD7606 thread start\r\n");
  AD7606_SPI4_Init();

  for (;;)
  {
    AD7606_SPI4_Task(HAL_GetTick());
    tx_thread_sleep(APP_AD7606_THREAD_SLEEP_TICKS);
  }
}

UINT App_AD7606_Start(TX_BYTE_POOL *byte_pool)
{
  UCHAR *thread_stack;
  UINT status;

  status = AppAD7606_ByteAllocate(byte_pool, &thread_stack, APP_AD7606_THREAD_STACK_SIZE);
  if (status != TX_SUCCESS)
  {
    return status;
  }

  status = tx_thread_create(&AppAd7606Thread,
                            "AD7606 SPI4 DMA",
                            AppAD7606_ThreadEntry,
                            0,
                            thread_stack,
                            APP_AD7606_THREAD_STACK_SIZE,
                            APP_AD7606_THREAD_PRIORITY,
                            APP_AD7606_THREAD_PRIORITY,
                            TX_NO_TIME_SLICE,
                            TX_AUTO_START);
  if (status != TX_SUCCESS)
  {
    App_PrintHex32("AD7606 thread create failed: ", status);
    return status;
  }

  App_Print("AD7606 thread created\r\n");
  return TX_SUCCESS;
}
