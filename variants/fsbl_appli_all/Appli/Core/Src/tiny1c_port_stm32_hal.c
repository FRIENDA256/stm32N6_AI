#include "tiny1c_port_stm32_hal.h"

#include "app_console.h"
#include "gpio.h"
#include "i2c.h"
#include "main.h"
#include "spi.h"
#include "tx_api.h"
#include "usart.h"

#include <stddef.h>
#include <string.h>

#define TINY1C_UART_TX_CHUNK_MAX 0xFFFFU
#define TINY1C_SPI3_USE_DMA      1U
#define TINY1C_SPI3_CHUNK_LEN    TINY1C_DEFAULT_SPI_CHUNK_LEN
#define TINY1C_SPI_TEST_DELAY_MS 40U
#define TINY1C_SPI_TEST_JUMP     512U
#define TINY1C_SPI_TEST_MAX_RUNS 100U

extern volatile ULONG _tx_thread_system_state;

static uint8_t tiny1c_spi_tx[TINY1C_SPI3_CHUNK_LEN];
static uint8_t tiny1c_spi_rx[TINY1C_SPI3_CHUNK_LEN];
static uint8_t tiny1c_frame[TINY1C_DEFAULT_FRAME_LEN];
static uint8_t tiny1c_initialized;
static volatile uint8_t tiny1c_spi_dma_active;
static volatile uint8_t tiny1c_spi_dma_done;
static volatile uint8_t tiny1c_spi_dma_error;
static volatile uint32_t tiny1c_spi_dma_hal_error;
static TX_SEMAPHORE tiny1c_spi_dma_semaphore;
static uint8_t tiny1c_spi_dma_semaphore_ready;
static volatile uint8_t tiny1c_spi_use_dma = TINY1C_SPI3_USE_DMA;

tiny1c_t g_tiny1c;

static ULONG Tiny1C_STM32_MsToTicks(uint32_t ms)
{
  ULONG ticks;

  ticks = (ULONG)(((uint64_t)ms * TX_TIMER_TICKS_PER_SECOND + 999ULL) / 1000ULL);
  return (ticks == 0UL) ? 1UL : ticks;
}

static uint32_t Tiny1C_STM32_SpiClockHz(void)
{
  uint32_t divider;

  switch (hspi3.Init.BaudRatePrescaler)
  {
    case SPI_BAUDRATEPRESCALER_2:
      divider = 2U;
      break;
    case SPI_BAUDRATEPRESCALER_4:
      divider = 4U;
      break;
    case SPI_BAUDRATEPRESCALER_8:
      divider = 8U;
      break;
    case SPI_BAUDRATEPRESCALER_16:
      divider = 16U;
      break;
    case SPI_BAUDRATEPRESCALER_32:
      divider = 32U;
      break;
    case SPI_BAUDRATEPRESCALER_64:
      divider = 64U;
      break;
    case SPI_BAUDRATEPRESCALER_128:
      divider = 128U;
      break;
    case SPI_BAUDRATEPRESCALER_256:
      divider = 256U;
      break;
    default:
      return 0U;
  }

  return HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SPI3) / divider;
}

static HAL_StatusTypeDef Tiny1C_STM32_SpiReconfigure(uint32_t prescaler)
{
  HAL_StatusTypeDef status;

  if (tiny1c_spi_dma_active != 0U)
  {
    return HAL_BUSY;
  }

  HAL_GPIO_WritePin(TINY1C_CS_GPIO_Port, TINY1C_CS_Pin, GPIO_PIN_SET);
  status = HAL_SPI_DeInit(&hspi3);
  if (status != HAL_OK)
  {
    return status;
  }

  hspi3.Init.BaudRatePrescaler = prescaler;
  return HAL_SPI_Init(&hspi3);
}

static HAL_StatusTypeDef Tiny1C_STM32_SpiDmaPriority(uint32_t priority)
{
  HAL_StatusTypeDef status;

  if ((hspi3.hdmarx == NULL) || (hspi3.hdmatx == NULL))
  {
    return HAL_ERROR;
  }

  hspi3.hdmarx->Init.Priority = priority;
  status = HAL_DMA_Init(hspi3.hdmarx);
  if (status != HAL_OK)
  {
    return status;
  }

  hspi3.hdmatx->Init.Priority = priority;
  return HAL_DMA_Init(hspi3.hdmatx);
}

static void Tiny1C_STM32_DelayMs(void *ctx, uint32_t ms)
{
  (void)ctx;

  if (ms == 0U)
  {
    return;
  }

  if (_tx_thread_system_state == 0U)
  {
    (void)tx_thread_sleep(Tiny1C_STM32_MsToTicks(ms));
  }
  else
  {
    HAL_Delay(ms);
  }
}

static uint32_t Tiny1C_STM32_TickMs(void *ctx)
{
  (void)ctx;
  return HAL_GetTick();
}

static int Tiny1C_STM32_UartWrite(void *ctx, const uint8_t *data, uint32_t len, uint32_t timeout_ms)
{
  uint32_t offset = 0U;

  (void)ctx;

  if ((data == NULL) && (len != 0U))
  {
    return -1;
  }

  while (offset < len)
  {
    uint32_t chunk = len - offset;
    if (chunk > TINY1C_UART_TX_CHUNK_MAX)
    {
      chunk = TINY1C_UART_TX_CHUNK_MAX;
    }

    if (HAL_UART_Transmit(&huart3,
                          (uint8_t *)&data[offset],
                          (uint16_t)chunk,
                          timeout_ms) != HAL_OK)
    {
      return -1;
    }

    offset += chunk;
  }

  return 0;
}

static int Tiny1C_STM32_I2cReady(void *ctx, uint16_t addr_8bit, uint32_t trials, uint32_t timeout_ms)
{
  (void)ctx;
  return (HAL_I2C_IsDeviceReady(&hi2c2, addr_8bit, trials, timeout_ms) == HAL_OK) ? 0 : -1;
}

static int Tiny1C_STM32_I2cMemRead(void *ctx,
                                   uint16_t addr_8bit,
                                   uint16_t reg,
                                   uint8_t *data,
                                   uint16_t len,
                                   uint32_t timeout_ms)
{
  (void)ctx;
  return (HAL_I2C_Mem_Read(&hi2c2,
                           addr_8bit,
                           reg,
                           I2C_MEMADD_SIZE_16BIT,
                           data,
                           len,
                           timeout_ms) == HAL_OK) ? 0 : -1;
}

static int Tiny1C_STM32_I2cMemWrite(void *ctx,
                                    uint16_t addr_8bit,
                                    uint16_t reg,
                                    const uint8_t *data,
                                    uint16_t len,
                                    uint32_t timeout_ms)
{
  (void)ctx;
  return (HAL_I2C_Mem_Write(&hi2c2,
                            addr_8bit,
                            reg,
                            I2C_MEMADD_SIZE_16BIT,
                            (uint8_t *)data,
                            len,
                            timeout_ms) == HAL_OK) ? 0 : -1;
}

static void Tiny1C_STM32_SpiCsWrite(void *ctx, uint8_t high)
{
  (void)ctx;
  HAL_GPIO_WritePin(TINY1C_CS_GPIO_Port, TINY1C_CS_Pin, (high != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static int Tiny1C_STM32_SpiTxRx(void *ctx,
                                const uint8_t *tx,
                                uint8_t *rx,
                                uint32_t len,
                                uint32_t timeout_ms)
{
#if (TINY1C_SPI3_USE_DMA == 1U)
  HAL_StatusTypeDef status;
  uint32_t start_tick;
  UINT wait_status;
#endif

  (void)ctx;

  if ((tx == NULL) || (rx == NULL) || (len > 0xFFFFU))
  {
    return -1;
  }

#if (TINY1C_SPI3_USE_DMA == 1U)
  if ((tiny1c_spi_use_dma != 0U) && (_tx_thread_system_state == 0U))
  {
    if (tiny1c_spi_dma_semaphore_ready != 0U)
    {
      while (tx_semaphore_get(&tiny1c_spi_dma_semaphore, TX_NO_WAIT) == TX_SUCCESS)
      {
      }
    }

    tiny1c_spi_dma_active = 1U;
    tiny1c_spi_dma_done = 0U;
    tiny1c_spi_dma_error = 0U;
    tiny1c_spi_dma_hal_error = 0U;

    status = HAL_SPI_TransmitReceive_DMA(&hspi3,
                                         (uint8_t *)tx,
                                         rx,
                                         (uint16_t)len);
    if (status == HAL_OK)
    {
      if (tiny1c_spi_dma_semaphore_ready != 0U)
      {
        wait_status = tx_semaphore_get(&tiny1c_spi_dma_semaphore,
                                       (timeout_ms == HAL_MAX_DELAY) ?
                                       TX_WAIT_FOREVER : Tiny1C_STM32_MsToTicks(timeout_ms));
        if (wait_status != TX_SUCCESS)
        {
          (void)HAL_SPI_Abort(&hspi3);
          tiny1c_spi_dma_active = 0U;
          return -1;
        }
      }
      else
      {
        start_tick = HAL_GetTick();
        while ((tiny1c_spi_dma_done == 0U) && (tiny1c_spi_dma_error == 0U))
        {
          if ((timeout_ms != HAL_MAX_DELAY) && ((HAL_GetTick() - start_tick) >= timeout_ms))
          {
            (void)HAL_SPI_Abort(&hspi3);
            tiny1c_spi_dma_active = 0U;
            return -1;
          }
          tx_thread_sleep(1U);
        }
      }

      tiny1c_spi_dma_active = 0U;
      return ((tiny1c_spi_dma_done != 0U) && (tiny1c_spi_dma_error == 0U)) ? 0 : -1;
    }

    tiny1c_spi_dma_active = 0U;
  }
#endif

  return (HAL_SPI_TransmitReceive(&hspi3,
                                  (uint8_t *)tx,
                                  rx,
                                  (uint16_t)len,
                                  timeout_ms) == HAL_OK) ? 0 : -1;
}

void Tiny1C_STM32_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
  if ((hspi == NULL) || (hspi->Instance != SPI3) || (tiny1c_spi_dma_active == 0U))
  {
    return;
  }

  tiny1c_spi_dma_done = 1U;
  if (tiny1c_spi_dma_semaphore_ready != 0U)
  {
    (void)tx_semaphore_put(&tiny1c_spi_dma_semaphore);
  }
}

void Tiny1C_STM32_ErrorCallback(SPI_HandleTypeDef *hspi)
{
  if ((hspi == NULL) || (hspi->Instance != SPI3) || (tiny1c_spi_dma_active == 0U))
  {
    return;
  }

  tiny1c_spi_dma_hal_error = HAL_SPI_GetError(hspi);
  tiny1c_spi_dma_error = 1U;
  if (tiny1c_spi_dma_semaphore_ready != 0U)
  {
    (void)tx_semaphore_put(&tiny1c_spi_dma_semaphore);
  }
}

static int Tiny1C_STM32_VsyncRead(void *ctx)
{
  (void)ctx;
  return (HAL_GPIO_ReadPin(TINY1C_VSYNC_GPIO_Port, TINY1C_VSYNC_Pin) == GPIO_PIN_SET) ? 1 : 0;
}

void Tiny1C_STM32_GpioInit(void)
{
  HAL_GPIO_WritePin(TINY1C_CS_GPIO_Port, TINY1C_CS_Pin, GPIO_PIN_SET);
}

tiny1c_status_t Tiny1C_STM32_Init(void)
{
  tiny1c_config_t config;
  tiny1c_port_t port = {0};
  tiny1c_buffers_t buffers = {0};
  tiny1c_status_t status;

  if (tiny1c_initialized != 0U)
  {
    return TINY1C_STATUS_OK;
  }

  if ((_tx_thread_system_state == 0U) && (tiny1c_spi_dma_semaphore_ready == 0U))
  {
    if (tx_semaphore_create(&tiny1c_spi_dma_semaphore, "Tiny1C SPI DMA", 0U) != TX_SUCCESS)
    {
      return TINY1C_STATUS_ERROR;
    }
    tiny1c_spi_dma_semaphore_ready = 1U;
  }

  Tiny1C_DefaultConfig(&config);
  config.spi_chunk_len = TINY1C_SPI3_CHUNK_LEN;
  config.use_direct_read = 1U;
  config.warmup_ms = 5000U;
  config.warmup_discard_frames = 2U;

  port.delay_ms = Tiny1C_STM32_DelayMs;
  port.tick_ms = Tiny1C_STM32_TickMs;
  port.uart_write = Tiny1C_STM32_UartWrite;
  port.i2c_is_ready = Tiny1C_STM32_I2cReady;
  port.i2c_mem_read = Tiny1C_STM32_I2cMemRead;
  port.i2c_mem_write = Tiny1C_STM32_I2cMemWrite;
  port.spi_cs_write = Tiny1C_STM32_SpiCsWrite;
  port.spi_txrx = Tiny1C_STM32_SpiTxRx;
  port.vsync_read = Tiny1C_STM32_VsyncRead;

  buffers.spi_tx = tiny1c_spi_tx;
  buffers.spi_rx = tiny1c_spi_rx;
  buffers.spi_buf_len = sizeof(tiny1c_spi_tx);
  buffers.frame = tiny1c_frame;
  buffers.frame_buf_len = sizeof(tiny1c_frame);
  buffers.flash_frame = NULL;
  buffers.flash_frame_buf_len = 0U;

  Tiny1C_STM32_GpioInit();
  status = Tiny1C_Init(&g_tiny1c, &config, &port, &buffers);
  if (status == TINY1C_STATUS_OK)
  {
    tiny1c_initialized = 1U;
    App_Print("Tiny1C driver ready\r\n");
    App_PrintHex32("Tiny1C SPI3 kernel Hz: ", HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SPI3));
    App_PrintHex32("Tiny1C SPI3 SCK Hz: ", Tiny1C_STM32_SpiClockHz());
  }

  return status;
}

tiny1c_status_t Tiny1C_STM32_ProcessCommand(uint8_t command)
{
  tiny1c_status_t status;
  uint8_t binary_dump_command;

  status = Tiny1C_STM32_Init();
  if (status != TINY1C_STATUS_OK)
  {
    App_Print("Tiny1C init failed\r\n");
    return status;
  }

  binary_dump_command = ((command == (uint8_t)'b') ||
                         (command == (uint8_t)'B') ||
                         (command == (uint8_t)'p') ||
                         (command == (uint8_t)'P')) ? 1U : 0U;

  if (binary_dump_command != 0U)
  {
    App_ConsoleSetMuted(1U);
  }

  status = Tiny1C_ProcessCommand(&g_tiny1c, command);

  if (binary_dump_command != 0U)
  {
    App_ConsoleSetMuted(0U);
  }

  return status;
}

tiny1c_status_t Tiny1C_STM32_CaptureFrame(uint8_t frame_command)
{
  tiny1c_status_t status;

  status = Tiny1C_STM32_Init();
  if (status != TINY1C_STATUS_OK)
  {
    App_Print("Tiny1C init failed\r\n");
    return status;
  }

  if (Tiny1C_CommandIsFrame(frame_command) == 0U)
  {
    return TINY1C_STATUS_UNSUPPORTED;
  }

  return Tiny1C_ReadFrame(&g_tiny1c, frame_command);
}

tiny1c_status_t Tiny1C_STM32_CaptureFrameBaseline(uint8_t frame_command)
{
  tiny1c_status_t status;

  status = Tiny1C_STM32_Init();
  if (status != TINY1C_STATUS_OK)
  {
    App_Print("Tiny1C init failed\r\n");
    return status;
  }

  if (Tiny1C_CommandIsFrame(frame_command) == 0U)
  {
    return TINY1C_STATUS_UNSUPPORTED;
  }

  return Tiny1C_ReadFrameBaseline(&g_tiny1c, frame_command);
}

tiny1c_status_t Tiny1C_STM32_CaptureFrameQuiet(uint8_t frame_command)
{
  tiny1c_status_t status;

  status = Tiny1C_STM32_Init();
  if (status != TINY1C_STATUS_OK)
  {
    return status;
  }

  if (Tiny1C_CommandIsFrame(frame_command) == 0U)
  {
    return TINY1C_STATUS_UNSUPPORTED;
  }

  return Tiny1C_ReadFrameQuiet(&g_tiny1c, frame_command);
}

tiny1c_status_t Tiny1C_STM32_RestartPreview(void)
{
  tiny1c_status_t status = Tiny1C_STM32_Init();

  if (status != TINY1C_STATUS_OK)
  {
    return status;
  }

  g_tiny1c.frame_valid = 0U;
  status = Tiny1C_PreviewStart(&g_tiny1c);
  if (status == TINY1C_STATUS_OK)
  {
    g_tiny1c.preview_started = 1U;
    g_tiny1c.warmup_discard_pending = (uint8_t)g_tiny1c.config.warmup_discard_frames;
  }
  else
  {
    g_tiny1c.preview_started = 0U;
  }

  return status;
}

static uint16_t Tiny1C_STM32_GetLe16(const uint8_t *data)
{
  return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static uint32_t Tiny1C_STM32_AbsDiffU16(uint16_t left, uint16_t right)
{
  return (left >= right) ? (uint32_t)(left - right) : (uint32_t)(right - left);
}

static void Tiny1C_STM32_AnalyzeTemperatureFrame(Tiny1C_STM32_SpiModeStats_t *stats)
{
  const uint8_t *frame = g_tiny1c.buffers.frame;
  uint32_t frame_jumps = 0U;
  uint32_t frame_max_delta = 0U;

  if ((stats == NULL) || (frame == NULL))
  {
    return;
  }

  for (uint32_t y = 0U; y < TINY1C_DEFAULT_FRAME_HEIGHT; y++)
  {
    for (uint32_t x = 0U; x < TINY1C_DEFAULT_FRAME_WIDTH; x++)
    {
      uint32_t offset = ((y * TINY1C_DEFAULT_FRAME_WIDTH) + x) * 2U;
      uint16_t pixel = Tiny1C_STM32_GetLe16(&frame[offset]);

      if (pixel == 0U)
      {
        continue;
      }

      if ((x + 1U) < TINY1C_DEFAULT_FRAME_WIDTH)
      {
        uint16_t right = Tiny1C_STM32_GetLe16(&frame[offset + 2U]);

        if (right != 0U)
        {
          uint32_t delta = Tiny1C_STM32_AbsDiffU16(pixel, right);
          if (delta > frame_max_delta)
          {
            frame_max_delta = delta;
          }
          if (delta > TINY1C_SPI_TEST_JUMP)
          {
            frame_jumps++;
          }
        }
      }

      if ((y + 1U) < TINY1C_DEFAULT_FRAME_HEIGHT)
      {
        uint16_t below = Tiny1C_STM32_GetLe16(
            &frame[offset + (TINY1C_DEFAULT_FRAME_WIDTH * 2U)]);

        if (below != 0U)
        {
          uint32_t delta = Tiny1C_STM32_AbsDiffU16(pixel, below);
          if (delta > frame_max_delta)
          {
            frame_max_delta = delta;
          }
          if (delta > TINY1C_SPI_TEST_JUMP)
          {
            frame_jumps++;
          }
        }
      }
    }
  }

  stats->total_jumps += frame_jumps;
  if (frame_jumps > stats->max_jumps)
  {
    stats->max_jumps = frame_jumps;
  }
  if (frame_max_delta > stats->max_delta)
  {
    stats->max_delta = frame_max_delta;
  }
  stats->last_crc32 = Tiny1C_Crc32(frame, TINY1C_DEFAULT_FRAME_LEN);
}

static void Tiny1C_STM32_RunSpiMode(uint8_t use_dma,
                                    uint32_t iterations,
                                    Tiny1C_STM32_SpiModeStats_t *stats)
{
  tiny1c_spi_use_dma = use_dma;

  for (uint32_t i = 0U; i < iterations; i++)
  {
    uint32_t start_ms = HAL_GetTick();
    tiny1c_status_t status = Tiny1C_STM32_CaptureFrameQuiet(TINY1C_CMD_TEMP);
    uint32_t elapsed_ms = HAL_GetTick() - start_ms;

    stats->total_ms += elapsed_ms;
    if (elapsed_ms > stats->max_ms)
    {
      stats->max_ms = elapsed_ms;
    }

    if (status == TINY1C_STATUS_OK)
    {
      stats->ok_count++;
      Tiny1C_STM32_AnalyzeTemperatureFrame(stats);
    }
    else
    {
      stats->error_count++;
      stats->last_hal_error = HAL_SPI_GetError(&hspi3) | tiny1c_spi_dma_hal_error;
      (void)HAL_SPI_Abort(&hspi3);
      HAL_GPIO_WritePin(TINY1C_CS_GPIO_Port, TINY1C_CS_Pin, GPIO_PIN_SET);
    }

    Tiny1C_STM32_DelayMs(NULL, TINY1C_SPI_TEST_DELAY_MS);
  }
}

static tiny1c_status_t Tiny1C_STM32_RunSpi50TestInternal(
    uint32_t iterations,
    uint8_t include_blocking,
    Tiny1C_STM32_SpiTestResult_t *result)
{
  uint32_t saved_prescaler;
  uint32_t saved_rx_priority;
  uint32_t saved_tx_priority;
  uint8_t saved_use_dma;
  HAL_StatusTypeDef status;
  tiny1c_status_t init_status;

  if ((result == NULL) || (iterations == 0U) || (iterations > TINY1C_SPI_TEST_MAX_RUNS))
  {
    return TINY1C_STATUS_ERROR;
  }

  (void)memset(result, 0, sizeof(*result));
  result->iterations = iterations;
  result->priority_hal_status = (uint32_t)HAL_ERROR;
  init_status = Tiny1C_STM32_Init();
  if (init_status != TINY1C_STATUS_OK)
  {
    return init_status;
  }

  saved_prescaler = hspi3.Init.BaudRatePrescaler;
  saved_rx_priority = (hspi3.hdmarx != NULL) ? hspi3.hdmarx->Init.Priority : DMA_LOW_PRIORITY_MID_WEIGHT;
  saved_tx_priority = (hspi3.hdmatx != NULL) ? hspi3.hdmatx->Init.Priority : DMA_LOW_PRIORITY_MID_WEIGHT;
  saved_use_dma = tiny1c_spi_use_dma;
  status = Tiny1C_STM32_SpiReconfigure(SPI_BAUDRATEPRESCALER_4);
  result->switch_hal_status = (uint32_t)status;
  result->test_hz = Tiny1C_STM32_SpiClockHz();

  if (status == HAL_OK)
  {
    if (include_blocking != 0U)
    {
      Tiny1C_STM32_RunSpiMode(0U, iterations, &result->blocking);
      Tiny1C_STM32_DelayMs(NULL, TINY1C_SPI_TEST_DELAY_MS);
    }
    status = Tiny1C_STM32_SpiDmaPriority(DMA_LOW_PRIORITY_HIGH_WEIGHT);
    result->priority_hal_status = (uint32_t)status;
    if (status == HAL_OK)
    {
      Tiny1C_STM32_RunSpiMode(1U, iterations, &result->dma);
    }
  }

  tiny1c_spi_use_dma = saved_use_dma;
  status = Tiny1C_STM32_SpiReconfigure(saved_prescaler);
  if (status == HAL_OK)
  {
    if ((hspi3.hdmarx == NULL) || (hspi3.hdmatx == NULL))
    {
      status = HAL_ERROR;
    }
    else
    {
      hspi3.hdmarx->Init.Priority = saved_rx_priority;
      if (HAL_DMA_Init(hspi3.hdmarx) != HAL_OK)
      {
        status = HAL_ERROR;
      }
      hspi3.hdmatx->Init.Priority = saved_tx_priority;
      if (HAL_DMA_Init(hspi3.hdmatx) != HAL_OK)
      {
        status = HAL_ERROR;
      }
    }
  }
  result->restore_hal_status = (uint32_t)status;
  result->restored_hz = Tiny1C_STM32_SpiClockHz();

  return ((result->switch_hal_status == (uint32_t)HAL_OK) &&
          (result->priority_hal_status == (uint32_t)HAL_OK) &&
          (result->restore_hal_status == (uint32_t)HAL_OK)) ?
         TINY1C_STATUS_OK : TINY1C_STATUS_ERROR;
}

tiny1c_status_t Tiny1C_STM32_RunSpi50Test(uint32_t iterations,
                                          Tiny1C_STM32_SpiTestResult_t *result)
{
  if (iterations > 10U)
  {
    return TINY1C_STATUS_ERROR;
  }

  return Tiny1C_STM32_RunSpi50TestInternal(iterations, 1U, result);
}

tiny1c_status_t Tiny1C_STM32_RunSpi50DmaTest(uint32_t iterations,
                                             Tiny1C_STM32_SpiTestResult_t *result)
{
  return Tiny1C_STM32_RunSpi50TestInternal(iterations, 0U, result);
}

tiny1c_status_t Tiny1C_STM32_GetLatestFrame(const uint8_t **data,
                                            uint32_t *len,
                                            uint8_t *frame_command,
                                            uint32_t *crc32)
{
  if ((data == NULL) || (len == NULL) ||
      (g_tiny1c.frame_valid == 0U) ||
      (g_tiny1c.buffers.frame == NULL) ||
      (g_tiny1c.config.frame_len == 0U))
  {
    return TINY1C_STATUS_ERROR;
  }

  *data = g_tiny1c.buffers.frame;
  *len = g_tiny1c.config.frame_len;
  if (frame_command != NULL)
  {
    *frame_command = g_tiny1c.last_frame_command;
  }
  if (crc32 != NULL)
  {
    *crc32 = Tiny1C_Crc32(g_tiny1c.buffers.frame, g_tiny1c.config.frame_len);
  }

  return TINY1C_STATUS_OK;
}

void Tiny1C_STM32_PrintBootMessage(void)
{
  App_Print("Tiny1C TCP commands: IRPROBE IRVSYNC IRIMG IRTEMP IRFASTIMG IRFASTTEMP\r\n");
}
