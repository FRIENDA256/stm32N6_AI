/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_tcp_command.h
  * @brief   Minimal NetX Duo TCP command service.
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef APP_TCP_COMMAND_H
#define APP_TCP_COMMAND_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nx_api.h"
#include "tx_api.h"

#define APP_TCP_COMMAND_PORT 5000U

UINT AppTcpCommand_Start(NX_IP *ip_ptr, NX_PACKET_POOL *packet_pool, TX_BYTE_POOL *byte_pool);

#ifdef __cplusplus
}
#endif

#endif /* APP_TCP_COMMAND_H */
