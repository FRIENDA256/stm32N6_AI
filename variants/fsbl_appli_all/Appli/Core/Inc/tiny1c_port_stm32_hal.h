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
tiny1c_status_t Tiny1C_STM32_CaptureFrame(uint8_t frame_command);
tiny1c_status_t Tiny1C_STM32_CaptureFrameBaseline(uint8_t frame_command);
tiny1c_status_t Tiny1C_STM32_GetLatestFrame(const uint8_t **data,
                                            uint32_t *len,
                                            uint8_t *frame_command,
                                            uint32_t *crc32);
void Tiny1C_STM32_PrintBootMessage(void);

#ifdef __cplusplus
}
#endif

#endif /* TINY1C_PORT_STM32_HAL_H */
