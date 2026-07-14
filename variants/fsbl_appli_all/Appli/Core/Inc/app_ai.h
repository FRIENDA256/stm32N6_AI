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

typedef struct
{
  uint32_t initialized;
  uint32_t ready;
  uint32_t fault;
  uint32_t weights_ok;
  uint32_t run_count;
  uint32_t skip_count;
  uint32_t window_reset_count;
  uint32_t copy_error_count;
  uint32_t run_error_count;
  uint32_t deadline_miss_count;
  uint32_t npu_clock_hz;
  uint32_t npuram_clock_hz;
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

UINT App_AI_Start(TX_BYTE_POOL *byte_pool);
void App_AI_GetStatus(App_AI_Status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* APP_AI_H */
