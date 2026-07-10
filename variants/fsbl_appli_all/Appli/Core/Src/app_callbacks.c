/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_callbacks.c
  * @brief   Shared HAL callback dispatch for merged application modules.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "ad7606_spi_dma.h"
#include "tiny1c_port_stm32_hal.h"

void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
  AD7606_SPI4_TxRxCpltCallback(hspi);
  Tiny1C_STM32_TxRxCpltCallback(hspi);
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
  AD7606_SPI4_ErrorCallback(hspi);
  Tiny1C_STM32_ErrorCallback(hspi);
}

void HAL_GPIO_EXTI_Rising_Callback(uint16_t GPIO_Pin)
{
  AD7606_SPI4_EXTI_RisingCallback(GPIO_Pin);
}
