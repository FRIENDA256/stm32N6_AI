/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_ai.h
  * @brief   ThreadX AI worker for the first AD7606 inference vertical slice.
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef APP_AI_H
#define APP_AI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "tx_api.h"

#include <stdint.h>

#define APP_AI_TARGET_RATE_HZ 50U
#define APP_AI_INPUT_WINDOW_POINTS 1024U
#define APP_AI_INPUT_CHANNELS      8U
#define APP_AI_INPUT_SAMPLE_BYTES  2U
#define APP_AI_INPUT_WINDOW_BYTES  \
  (APP_AI_INPUT_WINDOW_POINTS * APP_AI_INPUT_CHANNELS * APP_AI_INPUT_SAMPLE_BYTES)
#define APP_AI_SOURCE_FRAME_POINTS 512U
#define APP_AI_SOURCE_FRAME_BYTES  \
  (APP_AI_SOURCE_FRAME_POINTS * APP_AI_INPUT_CHANNELS * APP_AI_INPUT_SAMPLE_BYTES)

typedef struct
{
  uint32_t initialized;
  uint32_t ready;
  uint32_t fault;
  uint32_t weights_ok;
  uint32_t run_count;
  uint32_t skip_count;
  uint32_t window_reset_count;
  uint32_t input_frame_count;
  uint32_t warmup_count;
  uint32_t input_source_gap_count;
  uint32_t inference_gap_count;
  uint32_t copy_error_count;
  uint32_t run_error_count;
  uint32_t deadline_miss_count;
  uint32_t bundle_wait_count;
  uint32_t output_queue_depth;
  uint32_t output_queue_high_water;
  uint32_t output_queue_overflow_count;
  uint32_t npu_clock_hz;
  uint32_t npuram_clock_hz;
  uint32_t target_rate_hz;
  uint32_t inference_total_ms;
  uint32_t max_inference_ms;
  uint32_t last_run_error_ms;
  int32_t last_run_status;
  uint32_t last_frame_seq;
  uint32_t last_timestamp_ms;
  uint32_t last_sample_counter;
  uint32_t last_inference_ms;
  uint8_t last_top_index;
  int8_t last_output[4];
} App_AI_Status_t;

typedef struct
{
  App_AI_Status_t ai_status;
  /* The source block is sent once per AD7606 frame. */
  uint16_t source_points;
  uint8_t source_channels;
  uint8_t source_bytes_per_sample;
  uint32_t source_sample_bytes;
  uint64_t source_block_start;
  uint64_t source_block_end;
  /* AI still consumes the contiguous two-frame window below. */
  uint16_t window_points;
  uint32_t window_sample_bytes;
  uint64_t window_block_start;
  uint64_t window_block_end;
} App_AI_Bundle_Info_t;

UINT App_AI_Start(TX_BYTE_POOL *byte_pool);
void App_AI_GetStatus(App_AI_Status_t *status);
UINT App_AI_SetProcessingActive(uint8_t active, ULONG wait_ticks);
void App_AI_SetBundleConsumerActive(uint8_t active);
uint32_t App_AI_CopyLatestBundle(uint8_t *raw_window,
                                 uint32_t raw_capacity,
                                 App_AI_Bundle_Info_t *bundle_info);

#ifdef __cplusplus
}
#endif

#endif /* APP_AI_H */
