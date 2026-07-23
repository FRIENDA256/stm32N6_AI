/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_stream_telemetry.h
  * @brief   Continuous AD7606 and AI telemetry stream over UDP.
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef APP_STREAM_TELEMETRY_H
#define APP_STREAM_TELEMETRY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nx_api.h"
#include "tx_api.h"

#include <stdint.h>

#define APP_STREAM_TELEMETRY_PORT 5100U

typedef struct
{
  uint32_t initialized;
  uint32_t listening;
  uint32_t connected;
  uint32_t boot_session_id;
  uint32_t client_count;
  uint32_t disconnect_count;
  uint32_t message_count;
  uint32_t bundle_message_count;
  uint64_t payload_bytes;
  uint64_t wire_bytes;
  uint32_t allocation_fail_count;
  uint32_t send_fail_count;
  uint32_t copy_miss_count;
  uint32_t bundle_source_gap_count;
  uint32_t last_bundle_run;
  uint32_t last_bundle_ad_sequence;
  uint32_t packet_pool_total;
  uint32_t packet_pool_free;
  uint32_t packet_pool_empty_requests;
  uint32_t packet_pool_min_free;
} App_Stream_Telemetry_Status_t;

UINT AppStreamTelemetry_Start(NX_IP *ip_ptr, TX_BYTE_POOL *byte_pool);
void AppStreamTelemetry_GetStatus(App_Stream_Telemetry_Status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* APP_STREAM_TELEMETRY_H */
