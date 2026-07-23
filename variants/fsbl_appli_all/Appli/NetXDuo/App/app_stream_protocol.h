/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_stream_protocol.h
  * @brief   Portable encoder for the continuous multimodal stream protocol.
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef APP_STREAM_PROTOCOL_H
#define APP_STREAM_PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define APP_STREAM_PROTOCOL_VERSION       2U
#define APP_STREAM_PROTOCOL_HEADER_BYTES 64U

#define APP_STREAM_ID_AD7606_RAW          1U
#define APP_STREAM_ID_TINY1C_TEMP16       2U
#define APP_STREAM_ID_TINY1C_IMAGE16      3U
#define APP_STREAM_ID_IMX219_RGB565       4U
#define APP_STREAM_ID_IMX219_H264         5U
#define APP_STREAM_ID_AI_RESULT           6U
#define APP_STREAM_ID_RUNTIME_EVENT       7U
#define APP_STREAM_ID_AD_AI_BUNDLE        8U

#define APP_STREAM_FORMAT_AD7606_WIRE_V1  0x0101U
#define APP_STREAM_FORMAT_TINY1C_TEMP16_V1 0x0201U
#define APP_STREAM_FORMAT_TINY1C_IMAGE16_V1 0x0301U
#define APP_STREAM_FORMAT_AI_INT8_V1      0x0601U
#define APP_STREAM_FORMAT_AD_AI_BUNDLE_V1 0x0801U
#define APP_STREAM_FORMAT_AD_AI_BLOCK_V2  0x0802U

#define APP_STREAM_FLAG_PAYLOAD_CRC_VALID       (1UL << 0)
#define APP_STREAM_FLAG_SOURCE_TIMESTAMP_VALID  (1UL << 1)
#define APP_STREAM_FLAG_LATEST_SNAPSHOT         (1UL << 2)
#define APP_STREAM_FLAG_AI_REFERENCES_AD         (1UL << 3)

typedef struct
{
  uint16_t stream_id;
  uint16_t payload_format;
  uint32_t flags;
  uint32_t boot_session_id;
  uint32_t source_instance;
  uint64_t sequence;
  uint64_t capture_start_us;
  uint64_t capture_end_us;
  uint32_t payload_bytes;
  uint32_t aux0;
  uint32_t payload_crc32;
} App_Stream_Header_t;

uint32_t AppStreamProtocol_Crc32(const uint8_t *data, uint32_t length);
void AppStreamProtocol_EncodeHeader(uint8_t output[APP_STREAM_PROTOCOL_HEADER_BYTES],
                                    const App_Stream_Header_t *header);

#ifdef __cplusplus
}
#endif

#endif /* APP_STREAM_PROTOCOL_H */
