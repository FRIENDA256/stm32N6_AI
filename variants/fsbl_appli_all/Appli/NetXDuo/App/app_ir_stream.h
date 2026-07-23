/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_ir_stream.h
  * @brief   Independent Tiny1C latest-frame MMS2 stream over UDP.
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef APP_IR_STREAM_H
#define APP_IR_STREAM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nx_api.h"
#include "tx_api.h"

#include <stdint.h>

#define APP_IR_STREAM_PORT 5101U

typedef struct
{
  uint32_t initialized;
  uint32_t listening;
  uint32_t connected;
  uint32_t boot_session_id;
  uint32_t client_count;
  uint32_t disconnect_count;
  uint32_t message_count;
  uint32_t source_gap_count;
  uint64_t payload_bytes;
  uint64_t wire_bytes;
  uint32_t allocation_fail_count;
  uint32_t send_fail_count;
  uint32_t last_sequence;
  uint32_t last_timestamp_ms;
  uint32_t packet_pool_total;
  uint32_t packet_pool_free;
  uint32_t packet_pool_empty_requests;
  uint32_t packet_pool_min_free;
  uint32_t last_crc_ms;
  uint32_t last_send_ms;
  uint32_t max_send_ms;
  uint32_t tcp_state;
  uint32_t tcp_tx_queue_depth;
  uint32_t tcp_tx_window;
  uint32_t tcp_rx_window;
  uint32_t tcp_retransmit_packets;
  uint32_t send_active;
  uint32_t send_sequence;
  uint32_t send_stage;
  uint32_t send_start_ms;
} App_IRStream_Status_t;

UINT AppIRStream_Start(NX_IP *ip_ptr,
                       NX_PACKET_POOL *packet_pool,
                       TX_BYTE_POOL *byte_pool);
UINT AppIRStream_SetActive(uint8_t active);
void AppIRStream_GetStatus(App_IRStream_Status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* APP_IR_STREAM_H */
