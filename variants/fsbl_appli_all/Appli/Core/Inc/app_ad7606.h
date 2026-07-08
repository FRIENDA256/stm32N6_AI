/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_ad7606.h
  * @brief   ThreadX wrapper for the AD7606 SPI4 DMA acquisition path.
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef APP_AD7606_H
#define APP_AD7606_H

#ifdef __cplusplus
extern "C" {
#endif

#include "tx_api.h"

UINT App_AD7606_Start(TX_BYTE_POOL *byte_pool);

#ifdef __cplusplus
}
#endif

#endif /* APP_AD7606_H */
