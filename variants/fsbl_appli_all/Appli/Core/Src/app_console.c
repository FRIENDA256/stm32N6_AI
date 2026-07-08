/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_console.c
  * @brief   Small polled USART console helpers used during early bring-up.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "app_console.h"
#include "gpio.h"
#include "main.h"
#include "usart.h"

#define APP_UART_TX_WAIT_CYCLES 1000000U

static volatile uint32_t AppConsoleMuted;

static HAL_StatusTypeDef App_UartWaitFlag(uint32_t flag)
{
  if (huart3.Instance == NULL)
  {
    return HAL_ERROR;
  }

  for (volatile uint32_t wait = 0U; wait < APP_UART_TX_WAIT_CYCLES; wait++)
  {
    if ((huart3.Instance->ISR & flag) != 0U)
    {
      return HAL_OK;
    }
  }

  return HAL_TIMEOUT;
}

static HAL_StatusTypeDef App_UartWriteByte(uint8_t byte)
{
  if (huart3.Instance == NULL)
  {
    return HAL_ERROR;
  }

  huart3.Instance->ICR = USART_ICR_ORECF |
                         USART_ICR_FECF |
                         USART_ICR_NECF |
                         USART_ICR_PECF;

  if (App_UartWaitFlag(USART_ISR_TXE_TXFNF) != HAL_OK)
  {
    return HAL_TIMEOUT;
  }

  huart3.Instance->TDR = byte;
  return HAL_OK;
}

void App_Print(const char *text)
{
  if (AppConsoleMuted != 0U)
  {
    return;
  }

  if (text != NULL)
  {
    while (*text != '\0')
    {
      if (App_UartWriteByte((uint8_t)(*text)) != HAL_OK)
      {
        break;
      }
      text++;
    }
  }
}

void App_BootPrint(const char *text)
{
  HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
  App_Print(text);
  HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
}

void App_PrintHex16(const char *label, uint32_t value)
{
  static const char hex[] = "0123456789ABCDEF";
  char line[48];
  uint32_t pos = 0U;

  if (label != NULL)
  {
    while ((label[pos] != '\0') && (pos < (sizeof(line) - 9U)))
    {
      line[pos] = label[pos];
      pos++;
    }
  }

  line[pos++] = '0';
  line[pos++] = 'x';
  for (uint32_t nibble = 0U; nibble < 4U; nibble++)
  {
    uint32_t shift = 12U - (nibble * 4U);
    line[pos++] = hex[(value >> shift) & 0xFU];
  }
  line[pos++] = '\r';
  line[pos++] = '\n';
  line[pos] = '\0';

  App_Print(line);
}

void App_PrintHex32(const char *label, uint32_t value)
{
  static const char hex[] = "0123456789ABCDEF";
  char line[56];
  uint32_t pos = 0U;

  if (label != NULL)
  {
    while ((label[pos] != '\0') && (pos < (sizeof(line) - 13U)))
    {
      line[pos] = label[pos];
      pos++;
    }
  }

  line[pos++] = '0';
  line[pos++] = 'x';
  for (uint32_t nibble = 0U; nibble < 8U; nibble++)
  {
    uint32_t shift = 28U - (nibble * 4U);
    line[pos++] = hex[(value >> shift) & 0xFU];
  }
  line[pos++] = '\r';
  line[pos++] = '\n';
  line[pos] = '\0';

  App_Print(line);
}

void App_ConsoleSetMuted(uint32_t muted)
{
  AppConsoleMuted = (muted != 0U) ? 1U : 0U;
}
