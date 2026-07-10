/*
 * Minimal Sony IMX219 sensor driver for STM32 HAL I2C.
 *
 * Register programming is based on the upstream Linux IMX219 V4L2 driver
 * (drivers/media/i2c/imx219.c, GPL-2.0-only). Keep this file separated from
 * ST's BSP code if you need to review redistribution licensing later.
 */

#ifndef IMX219_H
#define IMX219_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32n6xx_hal.h"
#include <stddef.h>
#include <stdint.h>

#define IMX219_OK                       (0)
#define IMX219_ERROR                    (-1)

#define IMX219_I2C_ADDR_7BIT            (0x10U)
#define IMX219_CHIP_ID                  (0x0219U)

#define IMX219_RAW10                    (10U)
#define IMX219_R3280_2464               (0U)
#define IMX219_R1920_1080               (1U)
#define IMX219_R1640_1232               (2U)
#define IMX219_R640_480                 (3U)
#define IMX219_R320_240                 (4U)
#define IMX219_R640_480_BIN4            (5U)

typedef struct
{
  I2C_HandleTypeDef *I2cHandle;
  uint16_t Address;
  uint8_t IsInitialized;
  uint32_t Width;
  uint32_t Height;
} IMX219_Object_t;

int32_t IMX219_RegisterBusIO(IMX219_Object_t *pObj, I2C_HandleTypeDef *hi2c, uint16_t Address);
int32_t IMX219_ReadID(IMX219_Object_t *pObj, uint32_t *Id);
int32_t IMX219_EnterLp11(IMX219_Object_t *pObj);
int32_t IMX219_Init(IMX219_Object_t *pObj, uint32_t Resolution, uint32_t PixelFormat);
int32_t IMX219_Start(IMX219_Object_t *pObj);
int32_t IMX219_Stop(IMX219_Object_t *pObj);
int32_t IMX219_SetTestPattern(IMX219_Object_t *pObj, uint8_t Mode);
int32_t IMX219_SetFrameTiming(IMX219_Object_t *pObj, uint16_t LineLength, uint16_t FrameLength);
int32_t IMX219_SetExposureGain(IMX219_Object_t *pObj, uint16_t Exposure, uint8_t AnalogGain, uint16_t DigitalGain);
int32_t IMX219_SetDebugOpPllMultiplier(IMX219_Object_t *pObj, uint16_t Multiplier);
int32_t IMX219_SetCsiLaneMode(IMX219_Object_t *pObj, uint8_t LaneCount);
int32_t IMX219_ReadRegister8(IMX219_Object_t *pObj, uint16_t Reg, uint8_t *Value);
int32_t IMX219_ReadRegister16(IMX219_Object_t *pObj, uint16_t Reg, uint16_t *Value);

#ifdef __cplusplus
}
#endif

#endif /* IMX219_H */
