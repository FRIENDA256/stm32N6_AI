/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_console.h
  * @brief   Small polled USART console helpers used during early bring-up.
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef APP_CONSOLE_H
#define APP_CONSOLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32n6xx_hal.h"

void App_Print(const char *text);
void App_BootPrint(const char *text);
void App_PrintHex16(const char *label, uint32_t value);
void App_PrintHex32(const char *label, uint32_t value);
void App_ConsoleSetMuted(uint32_t muted);

#ifdef __cplusplus
}
#endif

#endif /* APP_CONSOLE_H */
