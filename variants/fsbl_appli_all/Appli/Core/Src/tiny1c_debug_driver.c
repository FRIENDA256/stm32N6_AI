#include "tiny1c_debug_driver.h"

#include <stddef.h>

static uint32_t Tiny1C_StrLen(const char *msg)
{
  uint32_t len = 0U;

  while ((msg != NULL) && (msg[len] != '\0'))
  {
    len++;
  }

  return len;
}

static void Tiny1C_Print(tiny1c_t *dev, const char *msg)
{
  if ((dev != NULL) && (dev->port.uart_write != NULL) && (msg != NULL))
  {
    (void)dev->port.uart_write(dev->port.ctx, (const uint8_t *)msg, Tiny1C_StrLen(msg), 100U);
  }
}

static void Tiny1C_PrintHex8(tiny1c_t *dev, uint8_t value)
{
  static const char hex[] = "0123456789ABCDEF";
  uint8_t out[2];

  out[0] = (uint8_t)hex[(value >> 4) & 0x0FU];
  out[1] = (uint8_t)hex[value & 0x0FU];
  if ((dev != NULL) && (dev->port.uart_write != NULL))
  {
    (void)dev->port.uart_write(dev->port.ctx, out, sizeof(out), 100U);
  }
}

static void Tiny1C_PrintHex16(tiny1c_t *dev, uint16_t value)
{
  Tiny1C_PrintHex8(dev, (uint8_t)((value >> 8) & 0xFFU));
  Tiny1C_PrintHex8(dev, (uint8_t)(value & 0xFFU));
}

static void Tiny1C_PrintHex32(tiny1c_t *dev, uint32_t value)
{
  Tiny1C_PrintHex16(dev, (uint16_t)((value >> 16) & 0xFFFFU));
  Tiny1C_PrintHex16(dev, (uint16_t)(value & 0xFFFFU));
}

static void Tiny1C_PrintU32(tiny1c_t *dev, uint32_t value)
{
  char out[10];
  uint32_t len = 0U;

  if (value == 0U)
  {
    Tiny1C_Print(dev, "0");
    return;
  }

  while ((value > 0U) && (len < sizeof(out)))
  {
    out[len] = (char)('0' + (value % 10U));
    value /= 10U;
    len++;
  }

  while (len > 0U)
  {
    len--;
    if (dev->port.uart_write != NULL)
    {
      (void)dev->port.uart_write(dev->port.ctx, (const uint8_t *)&out[len], 1U, 100U);
    }
  }
}

static void Tiny1C_Delay(tiny1c_t *dev, uint32_t ms)
{
  if ((dev != NULL) && (dev->port.delay_ms != NULL))
  {
    dev->port.delay_ms(dev->port.ctx, ms);
  }
}

static uint32_t Tiny1C_Tick(tiny1c_t *dev)
{
  if ((dev != NULL) && (dev->port.tick_ms != NULL))
  {
    return dev->port.tick_ms(dev->port.ctx);
  }

  return 0U;
}

static tiny1c_status_t Tiny1C_UARTSendBinary(tiny1c_t *dev, const uint8_t *data, uint32_t len)
{
  uint32_t offset = 0U;

  if ((dev == NULL) || (data == NULL) || (dev->port.uart_write == NULL))
  {
    return TINY1C_STATUS_ERROR;
  }

  while (offset < len)
  {
    uint32_t remain = len - offset;
    uint32_t chunk = (remain > dev->config.uart_bin_chunk) ? dev->config.uart_bin_chunk : remain;

    if (dev->port.uart_write(dev->port.ctx, &data[offset], chunk, 1000U) != 0)
    {
      return TINY1C_STATUS_ERROR;
    }
    offset += chunk;
    Tiny1C_Delay(dev, 1U);
  }

  return TINY1C_STATUS_OK;
}

static void Tiny1C_PutLe32(uint8_t *data, uint32_t value)
{
  data[0] = (uint8_t)(value & 0xFFU);
  data[1] = (uint8_t)((value >> 8) & 0xFFU);
  data[2] = (uint8_t)((value >> 16) & 0xFFU);
  data[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static uint32_t Tiny1C_GetLe32(const uint8_t *data)
{
  return ((uint32_t)data[0]) |
         ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) |
         ((uint32_t)data[3] << 24);
}

static tiny1c_status_t Tiny1C_WriteMem(tiny1c_t *dev, uint16_t reg, const uint8_t *data, uint16_t len)
{
  if ((dev == NULL) || (data == NULL) || (dev->port.i2c_mem_write == NULL))
  {
    return TINY1C_STATUS_ERROR;
  }

  return (dev->port.i2c_mem_write(dev->port.ctx, dev->config.i2c_addr_8bit, reg, data, len, 200U) == 0) ?
         TINY1C_STATUS_OK : TINY1C_STATUS_ERROR;
}

static tiny1c_status_t Tiny1C_SwitchFps(tiny1c_t *dev, uint8_t enable_25fps)
{
  const uint8_t switch_fps_cmd[] = {0x0FU, 0x08U, enable_25fps, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};

  return Tiny1C_WriteMem(dev, 0x1D00U, switch_fps_cmd, sizeof(switch_fps_cmd));
}

static tiny1c_status_t Tiny1C_SPIReadChunk(tiny1c_t *dev, uint8_t command, uint32_t chunk_len)
{
  if ((dev == NULL) ||
      (dev->buffers.spi_tx == NULL) ||
      (dev->buffers.spi_rx == NULL) ||
      (dev->buffers.spi_buf_len < chunk_len) ||
      (dev->port.spi_txrx == NULL) ||
      (dev->port.spi_cs_write == NULL))
  {
    return TINY1C_STATUS_ERROR;
  }

  for (uint32_t i = 0U; i < chunk_len; i++)
  {
    dev->buffers.spi_tx[i] = 0U;
  }
  dev->buffers.spi_tx[0] = command;

  dev->port.spi_cs_write(dev->port.ctx, 0U);
  if (dev->port.spi_txrx(dev->port.ctx, dev->buffers.spi_tx, dev->buffers.spi_rx, chunk_len, 1000U) == 0)
  {
    dev->port.spi_cs_write(dev->port.ctx, 1U);
    return TINY1C_STATUS_OK;
  }
  dev->port.spi_cs_write(dev->port.ctx, 1U);

  return TINY1C_STATUS_ERROR;
}

static tiny1c_status_t Tiny1C_SPIReadInto(tiny1c_t *dev, uint8_t command, uint8_t *rx_data, uint32_t transfer_len)
{
  if ((dev == NULL) ||
      (rx_data == NULL) ||
      (dev->buffers.spi_tx == NULL) ||
      (dev->buffers.spi_buf_len < transfer_len) ||
      (dev->port.spi_txrx == NULL) ||
      (dev->port.spi_cs_write == NULL))
  {
    return TINY1C_STATUS_ERROR;
  }

  for (uint32_t i = 0U; i < transfer_len; i++)
  {
    dev->buffers.spi_tx[i] = 0U;
  }
  dev->buffers.spi_tx[0] = command;

  dev->port.spi_cs_write(dev->port.ctx, 0U);
  if (dev->port.spi_txrx(dev->port.ctx, dev->buffers.spi_tx, rx_data, transfer_len, 1000U) == 0)
  {
    dev->port.spi_cs_write(dev->port.ctx, 1U);
    return TINY1C_STATUS_OK;
  }
  dev->port.spi_cs_write(dev->port.ctx, 1U);

  return TINY1C_STATUS_ERROR;
}

static tiny1c_status_t Tiny1C_ReadFrameRaw(tiny1c_t *dev, uint8_t first_command, uint8_t *frame_buffer)
{
  uint32_t copied_count = 0U;

  if ((dev == NULL) || (dev->config.spi_chunk_len == 0U) || (dev->config.frame_len == 0U))
  {
    return TINY1C_STATUS_ERROR;
  }

  while (copied_count < dev->config.frame_len)
  {
    uint8_t command = (copied_count == 0U) ? first_command : TINY1C_CMD_CONTINUE;
    uint32_t skip_len = (copied_count == 0U) ? dev->config.first_dummy_len : dev->config.continue_dummy_len;
    uint32_t available_len;
    uint32_t remain_len;
    uint32_t copy_len;

    if (skip_len >= dev->config.spi_chunk_len)
    {
      return TINY1C_STATUS_ERROR;
    }

    available_len = dev->config.spi_chunk_len - skip_len;
    remain_len = dev->config.frame_len - copied_count;
    copy_len = (remain_len < available_len) ? remain_len : available_len;

    if (Tiny1C_SPIReadChunk(dev, command, dev->config.spi_chunk_len) != TINY1C_STATUS_OK)
    {
      return TINY1C_STATUS_ERROR;
    }

    if (frame_buffer != NULL)
    {
      for (uint32_t i = 0U; i < copy_len; i++)
      {
        frame_buffer[copied_count + i] = dev->buffers.spi_rx[skip_len + i];
      }
    }

    copied_count += copy_len;
  }

  return TINY1C_STATUS_OK;
}

static tiny1c_status_t Tiny1C_ReadFrameRawDirect(tiny1c_t *dev, uint8_t first_command, uint8_t *frame_buffer)
{
  uint32_t copied_count = 0U;

  if ((dev == NULL) ||
      (frame_buffer == NULL) ||
      (dev->config.spi_chunk_len == 0U) ||
      (dev->config.frame_len == 0U))
  {
    return TINY1C_STATUS_ERROR;
  }

  while (copied_count < dev->config.frame_len)
  {
    uint8_t command = (copied_count == 0U) ? first_command : TINY1C_CMD_CONTINUE;
    uint32_t skip_len = (copied_count == 0U) ? dev->config.first_dummy_len : dev->config.continue_dummy_len;
    uint32_t available_len;
    uint32_t remain_len;
    uint32_t copy_len;
    uint32_t transfer_len;

    if (skip_len >= dev->config.spi_chunk_len)
    {
      return TINY1C_STATUS_ERROR;
    }

    available_len = dev->config.spi_chunk_len - skip_len;
    remain_len = dev->config.frame_len - copied_count;
    copy_len = (remain_len < available_len) ? remain_len : available_len;
    transfer_len = skip_len + copy_len;

    if (copied_count == 0U)
    {
      if (Tiny1C_SPIReadInto(dev, command, dev->buffers.spi_rx, transfer_len) != TINY1C_STATUS_OK)
      {
        return TINY1C_STATUS_ERROR;
      }
      for (uint32_t i = 0U; i < copy_len; i++)
      {
        frame_buffer[i] = dev->buffers.spi_rx[skip_len + i];
      }
    }
    else
    {
      uint8_t saved_previous = frame_buffer[copied_count - 1U];

      if (Tiny1C_SPIReadInto(dev, command, &frame_buffer[copied_count - 1U], transfer_len) != TINY1C_STATUS_OK)
      {
        return TINY1C_STATUS_ERROR;
      }
      frame_buffer[copied_count - 1U] = saved_previous;
    }

    copied_count += copy_len;
  }

  return TINY1C_STATUS_OK;
}

static tiny1c_status_t Tiny1C_FlashEnsureReady(tiny1c_t *dev)
{
  if (dev == NULL)
  {
    return TINY1C_STATUS_ERROR;
  }

  if ((dev->port.flash_init == NULL) ||
      (dev->port.flash_erase == NULL) ||
      (dev->port.flash_write == NULL) ||
      (dev->port.flash_read == NULL))
  {
    Tiny1C_Print(dev, "[Flash] flash callbacks not installed\r\n");
    return TINY1C_STATUS_UNSUPPORTED;
  }

  if (dev->flash_ready != 0U)
  {
    return TINY1C_STATUS_OK;
  }

  Tiny1C_Print(dev, "[Flash] EXTMEM init... ");
  if (dev->port.flash_init(dev->port.ctx) != 0)
  {
    Tiny1C_Print(dev, "FAIL\r\n");
    return TINY1C_STATUS_ERROR;
  }

  dev->flash_ready = 1U;
  Tiny1C_Print(dev, "OK\r\n");
  return TINY1C_STATUS_OK;
}

static uint32_t Tiny1C_FlashSlotAddr(tiny1c_t *dev, uint8_t command)
{
  if (command == TINY1C_CMD_TEMP)
  {
    return dev->config.flash_temp_slot_addr;
  }

  return dev->config.flash_image_slot_addr;
}

static void Tiny1C_PrintFrameStats(tiny1c_t *dev, uint8_t command, const char *frame_name)
{
  uint32_t zero_count = 0U;
  uint32_t ff_count = 0U;
  uint32_t other_count = 0U;
  uint32_t pixel_count = 0U;
  uint32_t nonzero_pixel_count = 0U;
  uint16_t min_pixel = 0xFFFFU;
  uint16_t max_pixel = 0x0000U;
  uint8_t first_data[16] = {0};
  uint8_t last_data[16] = {0};

  for (uint32_t frame_idx = 0U; frame_idx < dev->config.frame_len; frame_idx++)
  {
    uint8_t value = dev->buffers.frame[frame_idx];

    if (frame_idx < sizeof(first_data))
    {
      first_data[frame_idx] = value;
    }
    if (frame_idx >= (dev->config.frame_len - sizeof(last_data)))
    {
      last_data[frame_idx - (dev->config.frame_len - sizeof(last_data))] = value;
    }

    if (value == 0x00U)
    {
      zero_count++;
    }
    else if (value == 0xFFU)
    {
      ff_count++;
    }
    else
    {
      other_count++;
    }

    if ((frame_idx & 1U) != 0U)
    {
      uint8_t lo = dev->buffers.frame[frame_idx - 1U];
      uint8_t hi = value;
      uint16_t pixel = (uint16_t)lo | ((uint16_t)hi << 8);

      if (pixel < min_pixel)
      {
        min_pixel = pixel;
      }
      if (pixel > max_pixel)
      {
        max_pixel = pixel;
      }
      if (pixel != 0U)
      {
        nonzero_pixel_count++;
      }
      pixel_count++;
    }
  }

  dev->last_frame_command = command;
  dev->frame_valid = 1U;

  Tiny1C_Print(dev, "OK\r\n");
  Tiny1C_Print(dev, "[Tiny1C] first 16: ");
  for (uint32_t i = 0U; i < sizeof(first_data); i++)
  {
    Tiny1C_Print(dev, "0x");
    Tiny1C_PrintHex8(dev, first_data[i]);
    Tiny1C_Print(dev, " ");
  }
  Tiny1C_Print(dev, "\r\n");

  Tiny1C_Print(dev, "[Tiny1C] first 8 u16: ");
  for (uint32_t i = 0U; i < sizeof(first_data); i += 2U)
  {
    uint16_t pixel = (uint16_t)first_data[i] | ((uint16_t)first_data[i + 1U] << 8);
    Tiny1C_Print(dev, "0x");
    Tiny1C_PrintHex16(dev, pixel);
    Tiny1C_Print(dev, " ");
  }
  Tiny1C_Print(dev, "\r\n");

  Tiny1C_Print(dev, "[Tiny1C] last 16: ");
  for (uint32_t i = 0U; i < sizeof(last_data); i++)
  {
    Tiny1C_Print(dev, "0x");
    Tiny1C_PrintHex8(dev, last_data[i]);
    Tiny1C_Print(dev, " ");
  }
  Tiny1C_Print(dev, "\r\n");

  Tiny1C_Print(dev, "[Tiny1C] frame bytes: ");
  Tiny1C_PrintU32(dev, dev->config.frame_len);
  Tiny1C_Print(dev, "\r\n[Tiny1C] saved frame buffer: tiny1c_frame, command: 0x");
  Tiny1C_PrintHex8(dev, command);
  Tiny1C_Print(dev, "\r\n[Tiny1C] u16 pixels/nonzero/min/max: ");
  Tiny1C_PrintU32(dev, pixel_count);
  Tiny1C_Print(dev, " ");
  Tiny1C_PrintU32(dev, nonzero_pixel_count);
  Tiny1C_Print(dev, " 0x");
  Tiny1C_PrintHex16(dev, min_pixel);
  Tiny1C_Print(dev, " 0x");
  Tiny1C_PrintHex16(dev, max_pixel);
  Tiny1C_Print(dev, "\r\n[Tiny1C] zero/ff/other: ");
  Tiny1C_PrintU32(dev, zero_count);
  Tiny1C_Print(dev, " ");
  Tiny1C_PrintU32(dev, ff_count);
  Tiny1C_Print(dev, " ");
  Tiny1C_PrintU32(dev, other_count);
  Tiny1C_Print(dev, "\r\n");

  (void)frame_name;
}

void Tiny1C_DefaultConfig(tiny1c_config_t *config)
{
  if (config == NULL)
  {
    return;
  }

  config->i2c_addr_8bit = TINY1C_DEFAULT_I2C_ADDR_8BIT;
  config->spi_chunk_len = TINY1C_DEFAULT_SPI_CHUNK_LEN;
  config->first_dummy_len = TINY1C_DEFAULT_FIRST_DUMMY_LEN;
  config->continue_dummy_len = TINY1C_DEFAULT_CONTINUE_DUMMY_LEN;
  config->frame_width = TINY1C_DEFAULT_FRAME_WIDTH;
  config->frame_height = TINY1C_DEFAULT_FRAME_HEIGHT;
  config->frame_len = TINY1C_DEFAULT_FRAME_LEN;
  config->uart_bin_chunk = TINY1C_DEFAULT_UART_BIN_CHUNK;
  config->warmup_ms = TINY1C_DEFAULT_WARMUP_MS;
  config->warmup_discard_frames = TINY1C_DEFAULT_DISCARD_FRAMES;
  config->fast_test_frames = TINY1C_DEFAULT_FAST_TEST_FRAMES;
  config->enable_25fps_cmd = 1U;
  config->use_direct_read = 1U;
  config->flash_image_slot_addr = TINY1C_FLASH_IMAGE_SLOT_ADDR;
  config->flash_temp_slot_addr = TINY1C_FLASH_TEMP_SLOT_ADDR;
  config->flash_header_len = TINY1C_FLASH_HEADER_LEN;
}

tiny1c_status_t Tiny1C_Init(tiny1c_t *dev, const tiny1c_config_t *config, const tiny1c_port_t *port, const tiny1c_buffers_t *buffers)
{
  tiny1c_config_t default_config;

  if ((dev == NULL) || (port == NULL) || (buffers == NULL))
  {
    return TINY1C_STATUS_ERROR;
  }

  Tiny1C_DefaultConfig(&default_config);
  dev->config = (config != NULL) ? *config : default_config;
  dev->port = *port;
  dev->buffers = *buffers;

  if ((dev->buffers.spi_tx == NULL) ||
      (dev->buffers.spi_rx == NULL) ||
      (dev->buffers.frame == NULL) ||
      (dev->buffers.spi_buf_len < dev->config.spi_chunk_len) ||
      (dev->buffers.frame_buf_len < dev->config.frame_len) ||
      (dev->config.flash_header_len < 25U) ||
      (dev->config.flash_header_len > TINY1C_FLASH_HEADER_LEN))
  {
    return TINY1C_STATUS_ERROR;
  }

  dev->preview_started = 0U;
  dev->warmup_discard_pending = 0U;
  dev->last_frame_command = 0U;
  dev->frame_valid = 0U;
  dev->flash_ready = 0U;
  dev->last_flash_command = 0U;
  dev->flash_frame_valid = 0U;

  if ((dev->port.spi_cs_write != NULL))
  {
    dev->port.spi_cs_write(dev->port.ctx, 1U);
  }

  return TINY1C_STATUS_OK;
}

tiny1c_status_t Tiny1C_I2CProbe(tiny1c_t *dev)
{
  uint8_t reg0 = 0U;
  uint8_t reg1 = 0U;

  if ((dev == NULL) || (dev->port.i2c_is_ready == NULL) || (dev->port.i2c_mem_read == NULL))
  {
    return TINY1C_STATUS_ERROR;
  }

  Tiny1C_Print(dev, "[Tiny1C] I2C probe 0x3C... ");
  if (dev->port.i2c_is_ready(dev->port.ctx, dev->config.i2c_addr_8bit, 3U, 100U) == 0)
  {
    Tiny1C_Print(dev, "OK\r\n");
  }
  else
  {
    Tiny1C_Print(dev, "FAIL\r\n");
    return TINY1C_STATUS_ERROR;
  }

  Tiny1C_Print(dev, "[Tiny1C] read 0x0000/0x0001... ");
  if ((dev->port.i2c_mem_read(dev->port.ctx, dev->config.i2c_addr_8bit, 0x0000U, &reg0, 1U, 100U) == 0) &&
      (dev->port.i2c_mem_read(dev->port.ctx, dev->config.i2c_addr_8bit, 0x0001U, &reg1, 1U, 100U) == 0))
  {
    Tiny1C_Print(dev, "0x");
    Tiny1C_PrintHex8(dev, reg0);
    Tiny1C_Print(dev, " 0x");
    Tiny1C_PrintHex8(dev, reg1);
    Tiny1C_Print(dev, " (expect 0x53 0x52)\r\n");
    return TINY1C_STATUS_OK;
  }

  Tiny1C_Print(dev, "FAIL\r\n");
  return TINY1C_STATUS_ERROR;
}

tiny1c_status_t Tiny1C_PreviewStart(tiny1c_t *dev)
{
  const uint8_t stop_preview[] = {0x0FU, 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
  const uint8_t start_preview_0[] = {0x0FU, 0xC1U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x08U};
  const uint8_t start_preview_1[] = {0x00U, 0x00U, 0x01U, 0x00U, 0x01U, 0x80U, 0x19U, 0x08U};

  Tiny1C_Print(dev, "[Tiny1C] preview stop... ");
  if (Tiny1C_WriteMem(dev, 0x1D00U, stop_preview, sizeof(stop_preview)) == TINY1C_STATUS_OK)
  {
    Tiny1C_Print(dev, "OK\r\n");
  }
  else
  {
    Tiny1C_Print(dev, "FAIL\r\n");
    return TINY1C_STATUS_ERROR;
  }

  Tiny1C_Delay(dev, 100U);

  if (dev->config.enable_25fps_cmd != 0U)
  {
    Tiny1C_Print(dev, "[Tiny1C] switch 25fps... ");
    if (Tiny1C_SwitchFps(dev, 1U) == TINY1C_STATUS_OK)
    {
      Tiny1C_Print(dev, "OK\r\n");
    }
    else
    {
      Tiny1C_Print(dev, "FAIL\r\n");
      return TINY1C_STATUS_ERROR;
    }
    Tiny1C_Delay(dev, 100U);
  }

  Tiny1C_Print(dev, "[Tiny1C] preview start SPI 256x384... ");
  if ((Tiny1C_WriteMem(dev, 0x9D00U, start_preview_0, sizeof(start_preview_0)) == TINY1C_STATUS_OK) &&
      (Tiny1C_WriteMem(dev, 0x1D08U, start_preview_1, sizeof(start_preview_1)) == TINY1C_STATUS_OK))
  {
    Tiny1C_Print(dev, "OK\r\n");
  }
  else
  {
    Tiny1C_Print(dev, "FAIL\r\n");
    return TINY1C_STATUS_ERROR;
  }

  Tiny1C_Print(dev, "[Tiny1C] warmup ");
  Tiny1C_PrintU32(dev, dev->config.warmup_ms);
  Tiny1C_Print(dev, "ms...\r\n");
  Tiny1C_Delay(dev, dev->config.warmup_ms);

  return TINY1C_STATUS_OK;
}

tiny1c_status_t Tiny1C_EnsurePreviewStarted(tiny1c_t *dev)
{
  if (dev == NULL)
  {
    return TINY1C_STATUS_ERROR;
  }

  if (dev->preview_started != 0U)
  {
    Tiny1C_Print(dev, "[Tiny1C] preview already started\r\n");
    return TINY1C_STATUS_OK;
  }

  if (Tiny1C_PreviewStart(dev) == TINY1C_STATUS_OK)
  {
    dev->preview_started = 1U;
    dev->warmup_discard_pending = (uint8_t)dev->config.warmup_discard_frames;
    return TINY1C_STATUS_OK;
  }

  return TINY1C_STATUS_ERROR;
}

static tiny1c_status_t Tiny1C_ReadFrameWithMode(tiny1c_t *dev, uint8_t command, uint8_t direct_mode, uint8_t verbose)
{
  if ((dev == NULL) || (Tiny1C_CommandIsFrame(command) == 0U))
  {
    return TINY1C_STATUS_ERROR;
  }

  if (Tiny1C_EnsurePreviewStarted(dev) != TINY1C_STATUS_OK)
  {
    return TINY1C_STATUS_ERROR;
  }

  if (dev->warmup_discard_pending > 0U)
  {
    Tiny1C_Print(dev, "[Tiny1C] discard startup frames: ");
    Tiny1C_PrintU32(dev, dev->warmup_discard_pending);
    Tiny1C_Print(dev, "\r\n");

    while (dev->warmup_discard_pending > 0U)
    {
      Tiny1C_Print(dev, "[Tiny1C] discard frame... ");
      if (Tiny1C_ReadFrameRaw(dev, command, NULL) != TINY1C_STATUS_OK)
      {
        Tiny1C_Print(dev, "FAIL\r\n");
        return TINY1C_STATUS_ERROR;
      }
      dev->warmup_discard_pending--;
      Tiny1C_Print(dev, "OK\r\n");
      Tiny1C_Delay(dev, 30U);
    }
  }

  if (verbose != 0U)
  {
    Tiny1C_Print(dev, "[Tiny1C] SPI read ");
    Tiny1C_Print(dev, Tiny1C_FrameNameByCommand(command));
    Tiny1C_Print(dev, (direct_mode != 0U) ? " frame direct-trim " : " frame baseline ");
    Tiny1C_PrintU32(dev, dev->config.frame_len);
    Tiny1C_Print(dev, " bytes... chunk=");
    Tiny1C_PrintU32(dev, dev->config.spi_chunk_len);
    Tiny1C_Print(dev, (direct_mode != 0U) ? ", direct rx, skip=first512/cont1... " : ", cs=chunk, skip=first512/cont1... ");
  }

  if (((direct_mode != 0U) ?
       Tiny1C_ReadFrameRawDirect(dev, command, dev->buffers.frame) :
       Tiny1C_ReadFrameRaw(dev, command, dev->buffers.frame)) != TINY1C_STATUS_OK)
  {
    dev->frame_valid = 0U;
    if (verbose != 0U)
    {
      Tiny1C_Print(dev, "FAIL\r\n");
    }
    return TINY1C_STATUS_ERROR;
  }

  dev->last_frame_command = command;
  dev->frame_valid = 1U;

  if (verbose != 0U)
  {
    Tiny1C_PrintFrameStats(dev, command, Tiny1C_FrameNameByCommand(command));
  }
  return TINY1C_STATUS_OK;
}

tiny1c_status_t Tiny1C_ReadFrame(tiny1c_t *dev, uint8_t command)
{
  return Tiny1C_ReadFrameWithMode(dev, command, (dev != NULL) ? dev->config.use_direct_read : 0U, 1U);
}

tiny1c_status_t Tiny1C_ReadFrameBaseline(tiny1c_t *dev, uint8_t command)
{
  return Tiny1C_ReadFrameWithMode(dev, command, 0U, 1U);
}

tiny1c_status_t Tiny1C_ReadFrameDirect(tiny1c_t *dev, uint8_t command)
{
  return Tiny1C_ReadFrameWithMode(dev, command, 1U, 1U);
}

tiny1c_status_t Tiny1C_FastReadTest(tiny1c_t *dev, uint8_t command, uint32_t frame_count, uint8_t direct_mode)
{
  uint32_t start_tick;
  uint32_t elapsed_tick;
  uint32_t completed = 0U;
  uint32_t fps_x100 = 0U;

  if ((dev == NULL) || (Tiny1C_CommandIsFrame(command) == 0U) || (frame_count == 0U))
  {
    return TINY1C_STATUS_ERROR;
  }

  Tiny1C_Print(dev, "[Tiny1C] fast ");
  Tiny1C_Print(dev, Tiny1C_FrameNameByCommand(command));
  Tiny1C_Print(dev, (direct_mode != 0U) ? " direct-trim read test, frames=" : " baseline read test, frames=");
  Tiny1C_PrintU32(dev, frame_count);
  Tiny1C_Print(dev, "\r\n");

  if (Tiny1C_EnsurePreviewStarted(dev) != TINY1C_STATUS_OK)
  {
    return TINY1C_STATUS_ERROR;
  }

  if (dev->warmup_discard_pending > 0U)
  {
    Tiny1C_Print(dev, "[Tiny1C] discard startup frames: ");
    Tiny1C_PrintU32(dev, dev->warmup_discard_pending);
    Tiny1C_Print(dev, "\r\n");

    while (dev->warmup_discard_pending > 0U)
    {
      tiny1c_status_t status = (direct_mode != 0U) ?
        Tiny1C_ReadFrameRawDirect(dev, command, dev->buffers.frame) :
        Tiny1C_ReadFrameRaw(dev, command, NULL);

      if (status != TINY1C_STATUS_OK)
      {
        Tiny1C_Print(dev, "[Tiny1C] discard read FAIL\r\n");
        return TINY1C_STATUS_ERROR;
      }
      dev->warmup_discard_pending--;
      Tiny1C_Delay(dev, 30U);
    }
  }

  Tiny1C_Print(dev, "[Tiny1C] timed SPI read begin\r\n");
  start_tick = Tiny1C_Tick(dev);
  for (uint32_t i = 0U; i < frame_count; i++)
  {
    tiny1c_status_t status = (direct_mode != 0U) ?
      Tiny1C_ReadFrameRawDirect(dev, command, dev->buffers.frame) :
      Tiny1C_ReadFrameRaw(dev, command, dev->buffers.frame);

    if (status != TINY1C_STATUS_OK)
    {
      Tiny1C_Print(dev, "[Tiny1C] fast read FAIL at frame ");
      Tiny1C_PrintU32(dev, i);
      Tiny1C_Print(dev, "\r\n");
      break;
    }
    completed++;
  }
  elapsed_tick = Tiny1C_Tick(dev) - start_tick;

  if (completed == 0U)
  {
    dev->frame_valid = 0U;
    Tiny1C_Print(dev, "[Tiny1C] fast read FAIL, no complete frame\r\n");
    return TINY1C_STATUS_ERROR;
  }

  dev->last_frame_command = command;
  dev->frame_valid = 1U;

  if (elapsed_tick != 0U)
  {
    fps_x100 = (completed * 100000U) / elapsed_tick;
  }

  Tiny1C_Print(dev, "[Tiny1C] fast result frames/elapsed_ms/fps: ");
  Tiny1C_PrintU32(dev, completed);
  Tiny1C_Print(dev, "/");
  Tiny1C_PrintU32(dev, elapsed_tick);
  Tiny1C_Print(dev, "/");
  Tiny1C_PrintU32(dev, fps_x100 / 100U);
  Tiny1C_Print(dev, ".");
  if ((fps_x100 % 100U) < 10U)
  {
    Tiny1C_Print(dev, "0");
  }
  Tiny1C_PrintU32(dev, fps_x100 % 100U);
  Tiny1C_Print(dev, " FPS\r\n");

  Tiny1C_Print(dev, "[Tiny1C] last frame command/crc32: 0x");
  Tiny1C_PrintHex8(dev, command);
  Tiny1C_Print(dev, " 0x");
  Tiny1C_PrintHex32(dev, Tiny1C_Crc32(dev->buffers.frame, dev->config.frame_len));
  Tiny1C_Print(dev, "\r\n");

  return (completed == frame_count) ? TINY1C_STATUS_OK : TINY1C_STATUS_ERROR;
}

tiny1c_status_t Tiny1C_DumpCurrentFrameHex(tiny1c_t *dev)
{
  static const char hex[] = "0123456789ABCDEF";
  char line[(32U * 2U) + 2U];

  if ((dev == NULL) || (dev->frame_valid == 0U) || (dev->last_frame_command == 0U))
  {
    Tiny1C_Print(dev, "[Tiny1C] no saved frame, send 's' or 't' first\r\n");
    return TINY1C_STATUS_ERROR;
  }

  Tiny1C_Print(dev, "[Tiny1C] dump frame begin\r\n[Tiny1C] command: 0x");
  Tiny1C_PrintHex8(dev, dev->last_frame_command);
  Tiny1C_Print(dev, "\r\n[Tiny1C] bytes: ");
  Tiny1C_PrintU32(dev, dev->config.frame_len);
  Tiny1C_Print(dev, "\r\nBEGIN_TINY1C_FRAME\r\n");

  for (uint32_t offset = 0U; offset < dev->config.frame_len; offset += 32U)
  {
    for (uint32_t i = 0U; i < 32U; i++)
    {
      uint8_t value = dev->buffers.frame[offset + i];
      line[i * 2U] = hex[(value >> 4) & 0x0FU];
      line[(i * 2U) + 1U] = hex[value & 0x0FU];
    }
    line[64] = '\r';
    line[65] = '\n';
    if (dev->port.uart_write != NULL)
    {
      (void)dev->port.uart_write(dev->port.ctx, (const uint8_t *)line, sizeof(line), 1000U);
    }
  }

  Tiny1C_Print(dev, "END_TINY1C_FRAME\r\n[Tiny1C] dump frame end\r\n");
  return TINY1C_STATUS_OK;
}

static tiny1c_status_t Tiny1C_DumpFrameBinaryFromBuffer(tiny1c_t *dev, const uint8_t *data, uint8_t command, const char *tag)
{
  uint32_t crc;

  if ((dev == NULL) || (data == NULL) || (Tiny1C_CommandIsFrame(command) == 0U))
  {
    return TINY1C_STATUS_ERROR;
  }

  crc = Tiny1C_Crc32(data, dev->config.frame_len);
  Tiny1C_Print(dev, "[Tiny1C] binary frame begin\r\nTINY1C_BIN V1 CMD=0x");
  Tiny1C_PrintHex8(dev, command);
  Tiny1C_Print(dev, " WIDTH=");
  Tiny1C_PrintU32(dev, dev->config.frame_width);
  Tiny1C_Print(dev, " HEIGHT=");
  Tiny1C_PrintU32(dev, dev->config.frame_height);
  Tiny1C_Print(dev, " BYTES=");
  Tiny1C_PrintU32(dev, dev->config.frame_len);
  Tiny1C_Print(dev, " SOURCE=");
  Tiny1C_Print(dev, tag);
  Tiny1C_Print(dev, " CRC32=0x");
  Tiny1C_PrintHex32(dev, crc);
  Tiny1C_Print(dev, "\r\nBEGIN_TINY1C_BINARY\r\n");

  if (Tiny1C_UARTSendBinary(dev, data, dev->config.frame_len) != TINY1C_STATUS_OK)
  {
    Tiny1C_Print(dev, "\r\n[Tiny1C] binary uart TX fail\r\n");
    return TINY1C_STATUS_ERROR;
  }

  Tiny1C_Print(dev, "\r\nEND_TINY1C_BINARY\r\n[Tiny1C] binary frame end\r\n");
  return TINY1C_STATUS_OK;
}

tiny1c_status_t Tiny1C_DumpCurrentFrameBinary(tiny1c_t *dev)
{
  if ((dev == NULL) || (dev->frame_valid == 0U) || (dev->last_frame_command == 0U))
  {
    Tiny1C_Print(dev, "[Tiny1C] no saved frame, send 's' or 't' first\r\n");
    return TINY1C_STATUS_ERROR;
  }

  return Tiny1C_DumpFrameBinaryFromBuffer(dev, dev->buffers.frame, dev->last_frame_command, "RAM");
}

tiny1c_status_t Tiny1C_VsyncSample(tiny1c_t *dev, uint32_t sample_ms)
{
  uint32_t start_tick;
  uint32_t rising_count = 0U;
  uint32_t falling_count = 0U;
  int last_state;

  if ((dev == NULL) || (dev->port.vsync_read == NULL) || (dev->port.tick_ms == NULL))
  {
    Tiny1C_Print(dev, "[Tiny1C] VSYNC callback not installed\r\n");
    return TINY1C_STATUS_UNSUPPORTED;
  }

  start_tick = Tiny1C_Tick(dev);
  last_state = dev->port.vsync_read(dev->port.ctx);
  Tiny1C_Print(dev, "[Tiny1C] VSYNC sample ");
  Tiny1C_PrintU32(dev, sample_ms);
  Tiny1C_Print(dev, "ms, initial=");
  Tiny1C_Print(dev, (last_state != 0) ? "H" : "L");
  Tiny1C_Print(dev, "... ");

  while ((Tiny1C_Tick(dev) - start_tick) < sample_ms)
  {
    int state = dev->port.vsync_read(dev->port.ctx);

    if (state != last_state)
    {
      if (state != 0)
      {
        rising_count++;
      }
      else
      {
        falling_count++;
      }
      last_state = state;
    }
  }

  Tiny1C_Print(dev, "rising/falling/final: ");
  Tiny1C_PrintU32(dev, rising_count);
  Tiny1C_Print(dev, " ");
  Tiny1C_PrintU32(dev, falling_count);
  Tiny1C_Print(dev, " ");
  Tiny1C_Print(dev, (last_state != 0) ? "H" : "L");
  Tiny1C_Print(dev, "\r\n");
  return TINY1C_STATUS_OK;
}

tiny1c_status_t Tiny1C_FlashInitTest(tiny1c_t *dev)
{
  tiny1c_status_t status = Tiny1C_FlashEnsureReady(dev);

  if (status != TINY1C_STATUS_OK)
  {
    return status;
  }

  Tiny1C_Print(dev, "[Flash] image slot=0x");
  Tiny1C_PrintHex32(dev, dev->config.flash_image_slot_addr);
  Tiny1C_Print(dev, " temp slot=0x");
  Tiny1C_PrintHex32(dev, dev->config.flash_temp_slot_addr);
  Tiny1C_Print(dev, " total=");
  Tiny1C_PrintU32(dev, dev->config.flash_header_len + dev->config.frame_len);
  Tiny1C_Print(dev, "\r\n");
  return TINY1C_STATUS_OK;
}

static tiny1c_status_t Tiny1C_FlashWriteFrame(tiny1c_t *dev, uint8_t command)
{
  uint8_t header[TINY1C_FLASH_HEADER_LEN] = {0};
  uint32_t slot_addr;
  uint32_t crc;
  tiny1c_status_t status;

  if ((dev == NULL) || (Tiny1C_CommandIsFrame(command) == 0U))
  {
    Tiny1C_Print(dev, "[Flash] no selected RAM frame, send 's' or 't' first\r\n");
    return TINY1C_STATUS_ERROR;
  }

  if ((dev->frame_valid == 0U) || (dev->last_frame_command != command))
  {
    Tiny1C_Print(dev, "[Flash] current RAM frame is not ");
    Tiny1C_Print(dev, Tiny1C_FrameNameByCommand(command));
    Tiny1C_Print(dev, " frame, send '");
    Tiny1C_Print(dev, (command == TINY1C_CMD_TEMP) ? "t" : "s");
    Tiny1C_Print(dev, "' first\r\n");
    return TINY1C_STATUS_ERROR;
  }

  status = Tiny1C_FlashEnsureReady(dev);
  if (status != TINY1C_STATUS_OK)
  {
    return status;
  }

  slot_addr = Tiny1C_FlashSlotAddr(dev, command);
  crc = Tiny1C_Crc32(dev->buffers.frame, dev->config.frame_len);
  Tiny1C_PutLe32(&header[0], TINY1C_FLASH_MAGIC);
  Tiny1C_PutLe32(&header[4], TINY1C_FLASH_VERSION);
  Tiny1C_PutLe32(&header[8], dev->config.frame_width);
  Tiny1C_PutLe32(&header[12], dev->config.frame_height);
  Tiny1C_PutLe32(&header[16], dev->config.frame_len);
  Tiny1C_PutLe32(&header[20], crc);
  header[24] = command;

  Tiny1C_Print(dev, "[Flash] erase ");
  Tiny1C_Print(dev, Tiny1C_FrameNameByCommand(command));
  Tiny1C_Print(dev, " slot 0x");
  Tiny1C_PrintHex32(dev, slot_addr);
  Tiny1C_Print(dev, "... ");
  if (dev->port.flash_erase(dev->port.ctx, slot_addr, dev->config.flash_header_len + dev->config.frame_len) != 0)
  {
    Tiny1C_Print(dev, "FAIL\r\n");
    return TINY1C_STATUS_ERROR;
  }
  Tiny1C_Print(dev, "OK\r\n[Flash] write header... ");

  if (dev->port.flash_write(dev->port.ctx, slot_addr, header, dev->config.flash_header_len) != 0)
  {
    Tiny1C_Print(dev, "FAIL\r\n");
    return TINY1C_STATUS_ERROR;
  }
  Tiny1C_Print(dev, "OK\r\n[Flash] write frame ");
  Tiny1C_PrintU32(dev, dev->config.frame_len);
  Tiny1C_Print(dev, " bytes... ");

  if (dev->port.flash_write(dev->port.ctx, slot_addr + dev->config.flash_header_len, dev->buffers.frame, dev->config.frame_len) != 0)
  {
    Tiny1C_Print(dev, "FAIL\r\n");
    return TINY1C_STATUS_ERROR;
  }

  Tiny1C_Print(dev, "OK CRC32=0x");
  Tiny1C_PrintHex32(dev, crc);
  Tiny1C_Print(dev, "\r\n");
  return TINY1C_STATUS_OK;
}

tiny1c_status_t Tiny1C_FlashWriteCurrentFrame(tiny1c_t *dev)
{
  if (dev == NULL)
  {
    return TINY1C_STATUS_ERROR;
  }

  return Tiny1C_FlashWriteFrame(dev, dev->last_frame_command);
}

static tiny1c_status_t Tiny1C_FlashCaptureAndWriteFrame(tiny1c_t *dev, uint8_t command)
{
  if (Tiny1C_ReadFrame(dev, command) != TINY1C_STATUS_OK)
  {
    return TINY1C_STATUS_ERROR;
  }

  return Tiny1C_FlashWriteFrame(dev, command);
}

tiny1c_status_t Tiny1C_FlashCaptureAndWriteBoth(tiny1c_t *dev)
{
  Tiny1C_Print(dev, "[Flash] capture+write image and temp start\r\n");

  if (Tiny1C_FlashCaptureAndWriteFrame(dev, TINY1C_CMD_IMAGE) != TINY1C_STATUS_OK)
  {
    Tiny1C_Print(dev, "[Flash] image capture/write failed\r\n");
    return TINY1C_STATUS_ERROR;
  }

  Tiny1C_Delay(dev, 30U);

  if (Tiny1C_FlashCaptureAndWriteFrame(dev, TINY1C_CMD_TEMP) != TINY1C_STATUS_OK)
  {
    Tiny1C_Print(dev, "[Flash] temp capture/write failed\r\n");
    return TINY1C_STATUS_ERROR;
  }

  Tiny1C_Print(dev, "[Flash] capture+write image and temp done\r\n");
  return TINY1C_STATUS_OK;
}

static tiny1c_status_t Tiny1C_FlashReadBackFrame(tiny1c_t *dev, uint8_t command)
{
  uint8_t header[TINY1C_FLASH_HEADER_LEN] = {0};
  uint32_t slot_addr;
  uint32_t magic;
  uint32_t version;
  uint32_t width;
  uint32_t height;
  uint32_t bytes;
  uint32_t saved_crc;
  uint32_t read_crc;
  tiny1c_status_t status;

  if ((dev == NULL) || (Tiny1C_CommandIsFrame(command) == 0U))
  {
    Tiny1C_Print(dev, "[Flash] no selected frame, send 's' or 't' first\r\n");
    return TINY1C_STATUS_ERROR;
  }

  if ((dev->buffers.flash_frame == NULL) || (dev->buffers.flash_frame_buf_len < dev->config.frame_len))
  {
    Tiny1C_Print(dev, "[Flash] flash readback buffer not installed\r\n");
    return TINY1C_STATUS_ERROR;
  }

  status = Tiny1C_FlashEnsureReady(dev);
  if (status != TINY1C_STATUS_OK)
  {
    return status;
  }

  slot_addr = Tiny1C_FlashSlotAddr(dev, command);
  Tiny1C_Print(dev, "[Flash] read ");
  Tiny1C_Print(dev, Tiny1C_FrameNameByCommand(command));
  Tiny1C_Print(dev, " header 0x");
  Tiny1C_PrintHex32(dev, slot_addr);
  Tiny1C_Print(dev, "... ");

  if (dev->port.flash_read(dev->port.ctx, slot_addr, header, dev->config.flash_header_len) != 0)
  {
    Tiny1C_Print(dev, "FAIL\r\n");
    return TINY1C_STATUS_ERROR;
  }
  Tiny1C_Print(dev, "OK\r\n");

  magic = Tiny1C_GetLe32(&header[0]);
  version = Tiny1C_GetLe32(&header[4]);
  width = Tiny1C_GetLe32(&header[8]);
  height = Tiny1C_GetLe32(&header[12]);
  bytes = Tiny1C_GetLe32(&header[16]);
  saved_crc = Tiny1C_GetLe32(&header[20]);

  Tiny1C_Print(dev, "[Flash] header magic=0x");
  Tiny1C_PrintHex32(dev, magic);
  Tiny1C_Print(dev, " ver=");
  Tiny1C_PrintU32(dev, version);
  Tiny1C_Print(dev, " cmd=0x");
  Tiny1C_PrintHex8(dev, header[24]);
  Tiny1C_Print(dev, " ");
  Tiny1C_PrintU32(dev, width);
  Tiny1C_Print(dev, "x");
  Tiny1C_PrintU32(dev, height);
  Tiny1C_Print(dev, " bytes=");
  Tiny1C_PrintU32(dev, bytes);
  Tiny1C_Print(dev, " crc=0x");
  Tiny1C_PrintHex32(dev, saved_crc);
  Tiny1C_Print(dev, "\r\n");

  if ((magic != TINY1C_FLASH_MAGIC) ||
      (version != TINY1C_FLASH_VERSION) ||
      (width != dev->config.frame_width) ||
      (height != dev->config.frame_height) ||
      (bytes != dev->config.frame_len) ||
      (header[24] != command))
  {
    Tiny1C_Print(dev, "[Flash] invalid ");
    Tiny1C_Print(dev, Tiny1C_FrameNameByCommand(command));
    Tiny1C_Print(dev, " header\r\n");
    dev->flash_frame_valid = 0U;
    return TINY1C_STATUS_ERROR;
  }

  Tiny1C_Print(dev, "[Flash] read frame... ");
  if (dev->port.flash_read(dev->port.ctx, slot_addr + dev->config.flash_header_len, dev->buffers.flash_frame, dev->config.frame_len) != 0)
  {
    Tiny1C_Print(dev, "FAIL\r\n");
    dev->flash_frame_valid = 0U;
    return TINY1C_STATUS_ERROR;
  }

  read_crc = Tiny1C_Crc32(dev->buffers.flash_frame, dev->config.frame_len);
  Tiny1C_Print(dev, "OK CRC32=0x");
  Tiny1C_PrintHex32(dev, read_crc);
  Tiny1C_Print(dev, (read_crc == saved_crc) ? " MATCH\r\n" : " MISMATCH\r\n");
  dev->last_flash_command = command;
  dev->flash_frame_valid = (read_crc == saved_crc) ? 1U : 0U;

  return (read_crc == saved_crc) ? TINY1C_STATUS_OK : TINY1C_STATUS_ERROR;
}

tiny1c_status_t Tiny1C_FlashReadBackCurrent(tiny1c_t *dev)
{
  if (dev == NULL)
  {
    return TINY1C_STATUS_ERROR;
  }

  return Tiny1C_FlashReadBackFrame(dev, dev->last_frame_command);
}

tiny1c_status_t Tiny1C_FlashReadBackBoth(tiny1c_t *dev)
{
  tiny1c_status_t image_status = Tiny1C_FlashReadBackFrame(dev, TINY1C_CMD_IMAGE);
  tiny1c_status_t temp_status = Tiny1C_FlashReadBackFrame(dev, TINY1C_CMD_TEMP);

  return ((image_status == TINY1C_STATUS_OK) && (temp_status == TINY1C_STATUS_OK)) ? TINY1C_STATUS_OK : TINY1C_STATUS_ERROR;
}

tiny1c_status_t Tiny1C_FlashDumpCurrentBinary(tiny1c_t *dev)
{
  if ((dev == NULL) || (Tiny1C_CommandIsFrame(dev->last_frame_command) == 0U))
  {
    Tiny1C_Print(dev, "[Flash] no selected frame, send 's' or 't' first\r\n");
    return TINY1C_STATUS_ERROR;
  }

  if ((dev->flash_frame_valid == 0U) || (dev->last_flash_command != dev->last_frame_command))
  {
    Tiny1C_Print(dev, "[Flash] no valid readback frame, send 'r' first\r\n");
    return TINY1C_STATUS_ERROR;
  }

  return Tiny1C_DumpFrameBinaryFromBuffer(dev, dev->buffers.flash_frame, dev->last_flash_command, "FLASH");
}

tiny1c_status_t Tiny1C_FlashDumpBothBinary(tiny1c_t *dev)
{
  if (Tiny1C_FlashReadBackFrame(dev, TINY1C_CMD_IMAGE) != TINY1C_STATUS_OK)
  {
    return TINY1C_STATUS_ERROR;
  }
  if (Tiny1C_DumpFrameBinaryFromBuffer(dev, dev->buffers.flash_frame, TINY1C_CMD_IMAGE, "FLASH") != TINY1C_STATUS_OK)
  {
    return TINY1C_STATUS_ERROR;
  }

  if (Tiny1C_FlashReadBackFrame(dev, TINY1C_CMD_TEMP) != TINY1C_STATUS_OK)
  {
    return TINY1C_STATUS_ERROR;
  }
  return Tiny1C_DumpFrameBinaryFromBuffer(dev, dev->buffers.flash_frame, TINY1C_CMD_TEMP, "FLASH");
}

tiny1c_status_t Tiny1C_ProcessCommand(tiny1c_t *dev, uint8_t command)
{
  if ((command == 'i') || (command == 'I'))
  {
    return Tiny1C_I2CProbe(dev);
  }
  if ((command == 's') || (command == 'S'))
  {
    Tiny1C_Print(dev, "[Tiny1C] image SPI test start\r\n");
    return Tiny1C_ReadFrame(dev, TINY1C_CMD_IMAGE);
  }
  if ((command == 't') || (command == 'T'))
  {
    Tiny1C_Print(dev, "[Tiny1C] temp SPI test start\r\n");
    return Tiny1C_ReadFrame(dev, TINY1C_CMD_TEMP);
  }
  if (command == 'h')
  {
    return Tiny1C_FastReadTest(dev, TINY1C_CMD_IMAGE, dev->config.fast_test_frames, 0U);
  }
  if (command == 'j')
  {
    return Tiny1C_FastReadTest(dev, TINY1C_CMD_TEMP, dev->config.fast_test_frames, 0U);
  }
  if (command == 'k')
  {
    return Tiny1C_FastReadTest(dev, TINY1C_CMD_IMAGE, dev->config.fast_test_frames, 1U);
  }
  if (command == 'l')
  {
    return Tiny1C_FastReadTest(dev, TINY1C_CMD_TEMP, dev->config.fast_test_frames, 1U);
  }
  if ((command == 'd') || (command == 'D'))
  {
    return Tiny1C_DumpCurrentFrameHex(dev);
  }
  if ((command == 'b') || (command == 'B'))
  {
    return Tiny1C_DumpCurrentFrameBinary(dev);
  }
  if ((command == 'f') || (command == 'F'))
  {
    return Tiny1C_FlashInitTest(dev);
  }
  if (command == 'w')
  {
    return Tiny1C_FlashWriteCurrentFrame(dev);
  }
  if (command == 'a')
  {
    return Tiny1C_FlashCaptureAndWriteBoth(dev);
  }
  if (command == 'r')
  {
    return Tiny1C_FlashReadBackCurrent(dev);
  }
  if (command == 'R')
  {
    return Tiny1C_FlashReadBackBoth(dev);
  }
  if (command == 'p')
  {
    return Tiny1C_FlashDumpCurrentBinary(dev);
  }
  if (command == 'P')
  {
    return Tiny1C_FlashDumpBothBinary(dev);
  }
  if ((command == 'v') || (command == 'V'))
  {
    return Tiny1C_VsyncSample(dev, 1000U);
  }

  return TINY1C_STATUS_UNSUPPORTED;
}

uint32_t Tiny1C_Crc32(const uint8_t *data, uint32_t len)
{
  uint32_t crc = 0xFFFFFFFFU;

  if (data == NULL)
  {
    return 0U;
  }

  for (uint32_t i = 0U; i < len; i++)
  {
    crc ^= data[i];
    for (uint32_t bit = 0U; bit < 8U; bit++)
    {
      if ((crc & 1U) != 0U)
      {
        crc = (crc >> 1U) ^ 0xEDB88320U;
      }
      else
      {
        crc >>= 1U;
      }
    }
  }

  return ~crc;
}

const char *Tiny1C_FrameNameByCommand(uint8_t command)
{
  return (command == TINY1C_CMD_TEMP) ? "temp" : "image";
}

uint8_t Tiny1C_CommandIsFrame(uint8_t command)
{
  return ((command == TINY1C_CMD_IMAGE) || (command == TINY1C_CMD_TEMP)) ? 1U : 0U;
}
