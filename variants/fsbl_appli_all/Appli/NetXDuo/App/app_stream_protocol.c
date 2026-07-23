/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_stream_protocol.c
  * @brief   Portable encoder for the continuous multimodal stream protocol.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "app_stream_protocol.h"

#include <string.h>

static void AppStreamProtocol_WriteU16BE(uint8_t *output, uint16_t value)
{
  output[0] = (uint8_t)(value >> 8);
  output[1] = (uint8_t)value;
}

static void AppStreamProtocol_WriteU32BE(uint8_t *output, uint32_t value)
{
  output[0] = (uint8_t)(value >> 24);
  output[1] = (uint8_t)(value >> 16);
  output[2] = (uint8_t)(value >> 8);
  output[3] = (uint8_t)value;
}

static void AppStreamProtocol_WriteU64BE(uint8_t *output, uint64_t value)
{
  AppStreamProtocol_WriteU32BE(output, (uint32_t)(value >> 32));
  AppStreamProtocol_WriteU32BE(output + 4, (uint32_t)value);
}

uint32_t AppStreamProtocol_Crc32(const uint8_t *data, uint32_t length)
{
  static const uint32_t crc32_nibble_table[16] =
  {
    0x00000000U, 0x1DB71064U, 0x3B6E20C8U, 0x26D930ACU,
    0x76DC4190U, 0x6B6B51F4U, 0x4DB26158U, 0x5005713CU,
    0xEDB88320U, 0xF00F9344U, 0xD6D6A3E8U, 0xCB61B38CU,
    0x9B64C2B0U, 0x86D3D2D4U, 0xA00AE278U, 0xBDBDF21CU
  };
  uint32_t crc = 0xFFFFFFFFU;

  if ((data == NULL) && (length != 0U))
  {
    return 0U;
  }

  for (uint32_t index = 0U; index < length; index++)
  {
    crc ^= data[index];
    crc = (crc >> 4U) ^ crc32_nibble_table[crc & 0x0FU];
    crc = (crc >> 4U) ^ crc32_nibble_table[crc & 0x0FU];
  }

  return crc ^ 0xFFFFFFFFU;
}

void AppStreamProtocol_EncodeHeader(uint8_t output[APP_STREAM_PROTOCOL_HEADER_BYTES],
                                    const App_Stream_Header_t *header)
{
  uint32_t header_crc;

  if ((output == NULL) || (header == NULL))
  {
    return;
  }

  (void)memset(output, 0, APP_STREAM_PROTOCOL_HEADER_BYTES);
  output[0] = 'M';
  output[1] = 'M';
  output[2] = 'S';
  output[3] = '2';
  AppStreamProtocol_WriteU16BE(&output[4], APP_STREAM_PROTOCOL_VERSION);
  AppStreamProtocol_WriteU16BE(&output[6], APP_STREAM_PROTOCOL_HEADER_BYTES);
  AppStreamProtocol_WriteU16BE(&output[8], header->stream_id);
  AppStreamProtocol_WriteU16BE(&output[10], header->payload_format);
  AppStreamProtocol_WriteU32BE(&output[12], header->flags);
  AppStreamProtocol_WriteU32BE(&output[16], header->boot_session_id);
  AppStreamProtocol_WriteU32BE(&output[20], header->source_instance);
  AppStreamProtocol_WriteU64BE(&output[24], header->sequence);
  AppStreamProtocol_WriteU64BE(&output[32], header->capture_start_us);
  AppStreamProtocol_WriteU64BE(&output[40], header->capture_end_us);
  AppStreamProtocol_WriteU32BE(&output[48], header->payload_bytes);
  AppStreamProtocol_WriteU32BE(&output[52], header->aux0);
  AppStreamProtocol_WriteU32BE(&output[56], header->payload_crc32);

  header_crc = AppStreamProtocol_Crc32(output, APP_STREAM_PROTOCOL_HEADER_BYTES - 4U);
  AppStreamProtocol_WriteU32BE(&output[60], header_crc);
}
