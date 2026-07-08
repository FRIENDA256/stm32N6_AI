#ifndef TINY1C_PORT_STM32_HAL_H
#define TINY1C_PORT_STM32_HAL_H

#include "tiny1c_debug_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

extern tiny1c_t g_tiny1c;

void Tiny1C_STM32_GpioInit(void);
tiny1c_status_t Tiny1C_STM32_Init(void);
tiny1c_status_t Tiny1C_STM32_ProcessCommand(uint8_t command);
void Tiny1C_STM32_PrintBootMessage(void);

#ifdef __cplusplus
}
#endif

#endif /* TINY1C_PORT_STM32_HAL_H */
