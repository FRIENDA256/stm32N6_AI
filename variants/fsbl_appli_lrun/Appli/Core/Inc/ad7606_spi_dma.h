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

void AD7606_SPI4_Init(void);
void AD7606_SPI4_Task(uint32_t now_tick);

#ifdef __cplusplus
}
#endif

#endif /* __AD7606_SPI_DMA_H__ */
