/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_udp_echo.h
  * @brief   Minimal NetX Duo UDP echo service.
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef APP_UDP_ECHO_H
#define APP_UDP_ECHO_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nx_api.h"
#include "tx_api.h"

#define APP_UDP_ECHO_PORT 5005U

UINT AppUdpEcho_Start(NX_IP *ip_ptr, TX_BYTE_POOL *byte_pool);

#ifdef __cplusplus
}
#endif

#endif /* APP_UDP_ECHO_H */
