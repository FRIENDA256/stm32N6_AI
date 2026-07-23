/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    ad7606_spi_dma.h
  * @brief   SPI4 DMA receiver and quality monitor for the AD7606 acquisition card.
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __AD7606_SPI_DMA_H__
#define __AD7606_SPI_DMA_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>

#define AD7606_SPI4_MAX_FRAME_SIZE 8240U
#define AD7606_SPI4_AI_WINDOW_POINTS 1024U
#define AD7606_SPI4_AI_WINDOW_BYTES  16384U
#define AD7606_SPI4_AI_FRAME_POINTS  512U
#define AD7606_SPI4_AI_FRAME_BYTES   8192U
#define AD7606_SPI4_AI_QUEUE_DEPTH   8U

typedef struct
{
  uint32_t irq_count;
  uint32_t frame_seq;
  uint32_t timestamp_ms;
  uint32_t sample_counter;
  uint32_t crc32;
  uint16_t total_len;
  uint16_t payload_len;
  uint8_t frame_type;
  uint8_t crc_ok;
} AD7606_SPI4_FrameInfo_t;

typedef struct
{
  uint16_t points;
  uint8_t channels;
  uint8_t bytes_per_sample;
  uint32_t sample_bytes;
  uint64_t block_start;
  uint64_t block_end;
} AD7606_SPI4_RawInfo_t;

typedef struct
{
  uint32_t raw_frame_count;
  uint32_t queue_enqueued_count;
  uint32_t queue_dequeued_count;
  uint32_t queue_overflow_count;
  uint32_t queue_depth;
  uint32_t queue_high_water;
  uint32_t source_seq_gap_count;
  uint32_t source_block_gap_count;
  uint32_t last_frame_seq;
  uint64_t last_block_start;
  uint64_t last_block_end;
} AD7606_SPI4_AIQueueStatus_t;

void AD7606_SPI4_Init(void);
void AD7606_SPI4_Task(uint32_t now_tick);
void AD7606_SPI4_RequestRawDump(void);
void AD7606_SPI4_SetPaused(uint8_t paused);
uint8_t AD7606_SPI4_IsPaused(void);
uint8_t AD7606_SPI4_IsIdle(void);
uint32_t AD7606_SPI4_CopyLatestFrame(uint8_t *dest, uint32_t dest_len, AD7606_SPI4_FrameInfo_t *info);
uint32_t AD7606_SPI4_CopyLatestRawSamples(uint8_t *dest,
                                          uint32_t dest_len,
                                          AD7606_SPI4_FrameInfo_t *frame_info,
                                          AD7606_SPI4_RawInfo_t *raw_info);
uint32_t AD7606_SPI4_CopyLatestRawWindow(uint8_t *dest,
                                         uint32_t dest_len,
                                         AD7606_SPI4_FrameInfo_t *frame_info,
                                         AD7606_SPI4_RawInfo_t *raw_info);
uint32_t AD7606_SPI4_DequeueAIFrame(uint8_t *dest,
                                    uint32_t dest_len,
                                    AD7606_SPI4_FrameInfo_t *frame_info,
                                    AD7606_SPI4_RawInfo_t *raw_info);
void AD7606_SPI4_GetAIQueueStatus(AD7606_SPI4_AIQueueStatus_t *status);
void AD7606_SPI4_TxRxCpltCallback(SPI_HandleTypeDef *hspi);
void AD7606_SPI4_ErrorCallback(SPI_HandleTypeDef *hspi);
void AD7606_SPI4_EXTI_RisingCallback(uint16_t GPIO_Pin);

#ifdef __cplusplus
}
#endif

#endif /* __AD7606_SPI_DMA_H__ */
