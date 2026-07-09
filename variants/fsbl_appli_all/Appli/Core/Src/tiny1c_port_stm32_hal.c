#include "tiny1c_port_stm32_hal.h"

#include "app_console.h"
#include "gpio.h"
#include "i2c.h"
#include "main.h"
#include "spi.h"
#include "tx_api.h"
#include "usart.h"

#include <stddef.h>

#define TINY1C_UART_TX_CHUNK_MAX 0xFFFFU

extern volatile ULONG _tx_thread_system_state;

static uint8_t tiny1c_spi_tx[TINY1C_DEFAULT_SPI_CHUNK_LEN];
static uint8_t tiny1c_spi_rx[TINY1C_DEFAULT_SPI_CHUNK_LEN];
static uint8_t tiny1c_frame[TINY1C_DEFAULT_FRAME_LEN];
static uint8_t tiny1c_initialized;

tiny1c_t g_tiny1c;

static ULONG Tiny1C_STM32_MsToTicks(uint32_t ms)
{
  ULONG ticks;

  ticks = (ULONG)(((uint64_t)ms * TX_TIMER_TICKS_PER_SECOND + 999ULL) / 1000ULL);
  return (ticks == 0UL) ? 1UL : ticks;
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
  HAL_StatusTypeDef status;
  uint32_t primask;

  (void)ctx;

  if ((tx == NULL) || (rx == NULL) || (len > 0xFFFFU))
  {
    return -1;
  }

  primask = __get_PRIMASK();
  __disable_irq();
  status = HAL_SPI_TransmitReceive(&hspi3,
                                   (uint8_t *)tx,
                                   rx,
                                   (uint16_t)len,
                                   timeout_ms);
  if (primask == 0U)
  {
    __enable_irq();
  }

  return (status == HAL_OK) ? 0 : -1;
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

  Tiny1C_DefaultConfig(&config);
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
