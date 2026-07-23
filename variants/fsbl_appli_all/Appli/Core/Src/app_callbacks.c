/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_callbacks.c
  * @brief   Shared HAL callback dispatch for merged application modules.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "ad7606_spi_dma.h"
#include "app_ad7606.h"
#include "tiny1c_port_stm32_hal.h"

void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
  AD7606_SPI4_TxRxCpltCallback(hspi);
  if ((hspi != NULL) && (hspi->Instance == SPI4))
  {
    App_AD7606_NotifyWorkFromISR();
  }
  Tiny1C_STM32_TxRxCpltCallback(hspi);
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
  AD7606_SPI4_ErrorCallback(hspi);
  if ((hspi != NULL) && (hspi->Instance == SPI4))
  {
    App_AD7606_NotifyWorkFromISR();
  }
  Tiny1C_STM32_ErrorCallback(hspi);
}

void HAL_SPI_SuspendCallback(SPI_HandleTypeDef *hspi)
{
  Tiny1C_STM32_SuspendCallback(hspi);
}

void HAL_GPIO_EXTI_Rising_Callback(uint16_t GPIO_Pin)
{
  AD7606_SPI4_EXTI_RisingCallback(GPIO_Pin);
  if (GPIO_Pin == AD_IRQ_Pin)
  {
    App_AD7606_NotifyWorkFromISR();
  }
}
