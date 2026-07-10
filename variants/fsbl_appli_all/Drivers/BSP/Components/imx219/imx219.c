/*
 * Minimal Sony IMX219 sensor driver for STM32 HAL I2C.
 *
 * Register programming is based on the upstream Linux IMX219 V4L2 driver
 * (drivers/media/i2c/imx219.c, GPL-2.0-only). Keep this file separated from
 * ST's BSP code if you need to review redistribution licensing later.
 */

#include "imx219.h"

#define IMX219_REG_MODE_SELECT          0x0100U
#define IMX219_MODE_STANDBY             0x00U
#define IMX219_MODE_STREAMING           0x01U

#define IMX219_REG_CSI_LANE_MODE        0x0114U
#define IMX219_CSI_1_LANE_MODE          0x00U
#define IMX219_CSI_2_LANE_MODE          0x01U
#define IMX219_REG_DPHY_CTRL            0x0128U

#define IMX219_REG_X_ADD_STA            0x0164U
#define IMX219_REG_X_ADD_END            0x0166U
#define IMX219_REG_Y_ADD_STA            0x0168U
#define IMX219_REG_Y_ADD_END            0x016AU
#define IMX219_REG_X_OUTPUT_SIZE        0x016CU
#define IMX219_REG_Y_OUTPUT_SIZE        0x016EU
#define IMX219_REG_X_ODD_INC            0x0170U
#define IMX219_REG_Y_ODD_INC            0x0171U
#define IMX219_REG_BINNING_MODE_H       0x0174U
#define IMX219_REG_BINNING_MODE_V       0x0175U
#define IMX219_REG_CSI_DATA_FORMAT_A    0x018CU
#define IMX219_REG_CSI_DATA_FORMAT_B    0x018DU
#define IMX219_REG_LINE_LENGTH_A        0x0162U
#define IMX219_REG_FRAME_LENGTH_A       0x0160U
#define IMX219_REG_EXPOSURE             0x015AU
#define IMX219_REG_ANALOG_GAIN          0x0157U
#define IMX219_REG_DIGITAL_GAIN         0x0158U
#define IMX219_REG_TEST_PATTERN         0x0600U
#define IMX219_REG_PLL_OP_MPY           0x030CU

#define IMX219_REG_CHIP_ID              0x0000U
#define IMX219_NATIVE_WIDTH             3296U
#define IMX219_NATIVE_HEIGHT            2480U
#define IMX219_ACTIVE_WIDTH             3280U
#define IMX219_ACTIVE_HEIGHT            2464U
#define IMX219_ACTIVE_LEFT              8U
#define IMX219_ACTIVE_TOP               8U
#define IMX219_DEFAULT_EXPOSURE         0x0640U
#define IMX219_DEFAULT_ANALOG_GAIN      0x00U
#define IMX219_DEFAULT_DIGITAL_GAIN     0x0100U

typedef struct
{
  uint16_t Addr;
  uint8_t Val;
} IMX219_Reg_t;

typedef struct
{
  uint32_t Resolution;
  uint16_t Width;
  uint16_t Height;
  uint16_t FrameLength;
  uint16_t LineLength;
  uint8_t WindowScale;
  uint8_t OddInc;
  uint8_t BinningMode;
} IMX219_Mode_t;

static const IMX219_Mode_t IMX219_Modes[] =
{
  { IMX219_R3280_2464,    3280U, 2464U, 3526U, 3448U, 1U, 0x01U, 0x00U },
  { IMX219_R1920_1080,    1920U, 1080U, 1763U, 3448U, 1U, 0x01U, 0x00U },
  { IMX219_R1640_1232,    1640U, 1232U, 1707U, 3560U, 2U, 0x03U, 0x03U },
  { IMX219_R640_480,       640U,  480U, 1707U, 3560U, 1U, 0x01U, 0x00U },
  { IMX219_R320_240,       320U,  240U, 1707U, 3560U, 2U, 0x03U, 0x03U },
  { IMX219_R640_480_BIN4,  640U,  480U, 1067U, 3560U, 4U, 0x01U, 0x02U },
};

static const IMX219_Reg_t IMX219_CommonRegs[] =
{
  {0x30ebU, 0x05U},
  {0x30ebU, 0x0cU},
  {0x300aU, 0xffU},
  {0x300bU, 0xffU},
  {0x30ebU, 0x05U},
  {0x30ebU, 0x09U},
  {0x0114U, IMX219_CSI_2_LANE_MODE},
  {0x0128U, 0x00U},
  {0x012aU, 0x18U},
  {0x012bU, 0x00U},
  {0x0301U, 0x05U},
  {0x0303U, 0x01U},
  {0x0304U, 0x03U},
  {0x0305U, 0x03U},
  {0x0306U, 0x00U},
  {0x0307U, 0x39U},
  {0x030bU, 0x01U},
  {0x030cU, 0x00U},
  {0x030dU, 0x72U},
  {0x455eU, 0x00U},
  {0x471eU, 0x4bU},
  {0x4767U, 0x0fU},
  {0x4750U, 0x14U},
  {0x4540U, 0x00U},
  {0x47b4U, 0x14U},
  {0x4713U, 0x30U},
  {0x478bU, 0x10U},
  {0x478fU, 0x10U},
  {0x4793U, 0x10U},
  {0x4797U, 0x0eU},
  {0x479bU, 0x0eU},
};

static int32_t IMX219_WriteReg8(IMX219_Object_t *pObj, uint16_t Reg, uint8_t Value);
static int32_t IMX219_WriteReg16(IMX219_Object_t *pObj, uint16_t Reg, uint16_t Value);
static int32_t IMX219_ReadReg16(IMX219_Object_t *pObj, uint16_t Reg, uint16_t *Value);
static int32_t IMX219_WriteTable(IMX219_Object_t *pObj, const IMX219_Reg_t *Regs, uint32_t Count);
static const IMX219_Mode_t *IMX219_FindMode(uint32_t Resolution);

int32_t IMX219_RegisterBusIO(IMX219_Object_t *pObj, I2C_HandleTypeDef *hi2c, uint16_t Address)
{
  if ((pObj == NULL) || (hi2c == NULL))
  {
    return IMX219_ERROR;
  }

  pObj->I2cHandle = hi2c;
  pObj->Address = Address;
  pObj->IsInitialized = 0U;
  pObj->Width = 0U;
  pObj->Height = 0U;

  return IMX219_OK;
}

int32_t IMX219_ReadID(IMX219_Object_t *pObj, uint32_t *Id)
{
  uint16_t value;

  if ((pObj == NULL) || (Id == NULL))
  {
    return IMX219_ERROR;
  }

  if (IMX219_ReadReg16(pObj, IMX219_REG_CHIP_ID, &value) != IMX219_OK)
  {
    return IMX219_ERROR;
  }

  *Id = value;
  return IMX219_OK;
}

int32_t IMX219_EnterLp11(IMX219_Object_t *pObj)
{
  if (IMX219_Start(pObj) != IMX219_OK)
  {
    return IMX219_ERROR;
  }

  HAL_Delay(1);

  if (IMX219_Stop(pObj) != IMX219_OK)
  {
    return IMX219_ERROR;
  }

  HAL_Delay(1);

  return IMX219_OK;
}

int32_t IMX219_Init(IMX219_Object_t *pObj, uint32_t Resolution, uint32_t PixelFormat)
{
  const IMX219_Mode_t *mode;
  uint32_t window_scale;
  uint32_t crop_width;
  uint32_t crop_height;
  uint32_t x_start;
  uint32_t y_start;
  uint32_t exposure;

  if ((pObj == NULL) || (PixelFormat != IMX219_RAW10))
  {
    return IMX219_ERROR;
  }

  mode = IMX219_FindMode(Resolution);
  if (mode == NULL)
  {
    return IMX219_ERROR;
  }

  if (IMX219_Stop(pObj) != IMX219_OK)
  {
    return IMX219_ERROR;
  }

  if (IMX219_WriteTable(pObj, IMX219_CommonRegs,
                        (uint32_t)(sizeof(IMX219_CommonRegs) / sizeof(IMX219_CommonRegs[0]))) != IMX219_OK)
  {
    return IMX219_ERROR;
  }

  window_scale = mode->WindowScale;
  crop_width = (uint32_t)mode->Width * window_scale;
  crop_height = (uint32_t)mode->Height * window_scale;
  x_start = (IMX219_NATIVE_WIDTH - crop_width) / 2U;
  y_start = (IMX219_NATIVE_HEIGHT - crop_height) / 2U;

  if ((x_start >= IMX219_ACTIVE_LEFT) && (y_start >= IMX219_ACTIVE_TOP))
  {
    x_start -= IMX219_ACTIVE_LEFT;
    y_start -= IMX219_ACTIVE_TOP;
  }

  if ((IMX219_WriteReg16(pObj, IMX219_REG_X_ADD_STA, (uint16_t)x_start) != IMX219_OK) ||
      (IMX219_WriteReg16(pObj, IMX219_REG_X_ADD_END, (uint16_t)(x_start + crop_width - 1U)) != IMX219_OK) ||
      (IMX219_WriteReg16(pObj, IMX219_REG_Y_ADD_STA, (uint16_t)y_start) != IMX219_OK) ||
      (IMX219_WriteReg16(pObj, IMX219_REG_Y_ADD_END, (uint16_t)(y_start + crop_height - 1U)) != IMX219_OK) ||
      (IMX219_WriteReg16(pObj, IMX219_REG_X_OUTPUT_SIZE, mode->Width) != IMX219_OK) ||
      (IMX219_WriteReg16(pObj, IMX219_REG_Y_OUTPUT_SIZE, mode->Height) != IMX219_OK) ||
      (IMX219_WriteReg8(pObj, IMX219_REG_X_ODD_INC, mode->OddInc) != IMX219_OK) ||
      (IMX219_WriteReg8(pObj, IMX219_REG_Y_ODD_INC, mode->OddInc) != IMX219_OK) ||
      (IMX219_WriteReg8(pObj, IMX219_REG_BINNING_MODE_H, mode->BinningMode) != IMX219_OK) ||
      (IMX219_WriteReg8(pObj, IMX219_REG_BINNING_MODE_V, mode->BinningMode) != IMX219_OK) ||
      (IMX219_WriteReg8(pObj, IMX219_REG_CSI_DATA_FORMAT_A, 0x0aU) != IMX219_OK) ||
      (IMX219_WriteReg8(pObj, IMX219_REG_CSI_DATA_FORMAT_B, 0x0aU) != IMX219_OK) ||
      (IMX219_WriteReg8(pObj, IMX219_REG_CSI_LANE_MODE, IMX219_CSI_2_LANE_MODE) != IMX219_OK) ||
      (IMX219_WriteReg8(pObj, IMX219_REG_DPHY_CTRL, 0x00U) != IMX219_OK) ||
      (IMX219_WriteReg16(pObj, IMX219_REG_LINE_LENGTH_A, mode->LineLength) != IMX219_OK) ||
      (IMX219_WriteReg16(pObj, IMX219_REG_FRAME_LENGTH_A, mode->FrameLength) != IMX219_OK))
  {
    return IMX219_ERROR;
  }

  exposure = IMX219_DEFAULT_EXPOSURE;
  if (exposure > ((uint32_t)mode->FrameLength - 4U))
  {
    exposure = (uint32_t)mode->FrameLength - 4U;
  }

  if ((IMX219_WriteReg16(pObj, IMX219_REG_EXPOSURE, (uint16_t)exposure) != IMX219_OK) ||
      (IMX219_WriteReg8(pObj, IMX219_REG_ANALOG_GAIN, IMX219_DEFAULT_ANALOG_GAIN) != IMX219_OK) ||
      (IMX219_WriteReg16(pObj, IMX219_REG_DIGITAL_GAIN, IMX219_DEFAULT_DIGITAL_GAIN) != IMX219_OK) ||
      (IMX219_SetTestPattern(pObj, 0U) != IMX219_OK))
  {
    return IMX219_ERROR;
  }

  pObj->IsInitialized = 1U;
  pObj->Width = mode->Width;
  pObj->Height = mode->Height;

  return IMX219_OK;
}

int32_t IMX219_Start(IMX219_Object_t *pObj)
{
  if (pObj == NULL)
  {
    return IMX219_ERROR;
  }

  return IMX219_WriteReg8(pObj, IMX219_REG_MODE_SELECT, IMX219_MODE_STREAMING);
}

int32_t IMX219_Stop(IMX219_Object_t *pObj)
{
  if (pObj == NULL)
  {
    return IMX219_ERROR;
  }

  return IMX219_WriteReg8(pObj, IMX219_REG_MODE_SELECT, IMX219_MODE_STANDBY);
}

int32_t IMX219_SetTestPattern(IMX219_Object_t *pObj, uint8_t Mode)
{
  if (pObj == NULL)
  {
    return IMX219_ERROR;
  }

  return IMX219_WriteReg16(pObj, IMX219_REG_TEST_PATTERN, (uint16_t)Mode);
}

int32_t IMX219_SetFrameTiming(IMX219_Object_t *pObj, uint16_t LineLength, uint16_t FrameLength)
{
  if ((pObj == NULL) || (LineLength == 0U) || (FrameLength == 0U))
  {
    return IMX219_ERROR;
  }

  if ((IMX219_WriteReg16(pObj, IMX219_REG_LINE_LENGTH_A, LineLength) != IMX219_OK) ||
      (IMX219_WriteReg16(pObj, IMX219_REG_FRAME_LENGTH_A, FrameLength) != IMX219_OK))
  {
    return IMX219_ERROR;
  }

  return IMX219_OK;
}

int32_t IMX219_SetExposureGain(IMX219_Object_t *pObj, uint16_t Exposure, uint8_t AnalogGain, uint16_t DigitalGain)
{
  if ((pObj == NULL) || (Exposure == 0U))
  {
    return IMX219_ERROR;
  }

  if ((IMX219_WriteReg8(pObj, IMX219_REG_ANALOG_GAIN, AnalogGain) != IMX219_OK) ||
      (IMX219_WriteReg16(pObj, IMX219_REG_EXPOSURE, Exposure) != IMX219_OK) ||
      (IMX219_WriteReg16(pObj, IMX219_REG_DIGITAL_GAIN, DigitalGain) != IMX219_OK))
  {
    return IMX219_ERROR;
  }

  return IMX219_OK;
}

int32_t IMX219_SetDebugOpPllMultiplier(IMX219_Object_t *pObj, uint16_t Multiplier)
{
  if ((pObj == NULL) || (Multiplier == 0U))
  {
    return IMX219_ERROR;
  }

  return IMX219_WriteReg16(pObj, IMX219_REG_PLL_OP_MPY, Multiplier);
}

int32_t IMX219_SetCsiLaneMode(IMX219_Object_t *pObj, uint8_t LaneCount)
{
  uint8_t value;

  if (pObj == NULL)
  {
    return IMX219_ERROR;
  }

  if (LaneCount == 1U)
  {
    value = IMX219_CSI_1_LANE_MODE;
  }
  else if (LaneCount == 2U)
  {
    value = IMX219_CSI_2_LANE_MODE;
  }
  else
  {
    return IMX219_ERROR;
  }

  return IMX219_WriteReg8(pObj, IMX219_REG_CSI_LANE_MODE, value);
}

int32_t IMX219_ReadRegister8(IMX219_Object_t *pObj, uint16_t Reg, uint8_t *Value)
{
  uint8_t addr[2];

  if ((pObj == NULL) || (pObj->I2cHandle == NULL) || (Value == NULL))
  {
    return IMX219_ERROR;
  }

  addr[0] = (uint8_t)(Reg >> 8);
  addr[1] = (uint8_t)(Reg & 0xffU);

  if (HAL_I2C_Master_Transmit(pObj->I2cHandle, (uint16_t)(pObj->Address << 1), addr, sizeof(addr),
                              HAL_MAX_DELAY) != HAL_OK)
  {
    return IMX219_ERROR;
  }

  if (HAL_I2C_Master_Receive(pObj->I2cHandle, (uint16_t)(pObj->Address << 1), Value, 1U,
                             HAL_MAX_DELAY) != HAL_OK)
  {
    return IMX219_ERROR;
  }

  return IMX219_OK;
}

int32_t IMX219_ReadRegister16(IMX219_Object_t *pObj, uint16_t Reg, uint16_t *Value)
{
  return IMX219_ReadReg16(pObj, Reg, Value);
}

static int32_t IMX219_WriteReg8(IMX219_Object_t *pObj, uint16_t Reg, uint8_t Value)
{
  uint8_t data[3];

  if ((pObj == NULL) || (pObj->I2cHandle == NULL))
  {
    return IMX219_ERROR;
  }

  data[0] = (uint8_t)(Reg >> 8);
  data[1] = (uint8_t)(Reg & 0xffU);
  data[2] = Value;

  return (HAL_I2C_Master_Transmit(pObj->I2cHandle, (uint16_t)(pObj->Address << 1), data, sizeof(data),
                                  HAL_MAX_DELAY) == HAL_OK) ? IMX219_OK : IMX219_ERROR;
}

static int32_t IMX219_WriteReg16(IMX219_Object_t *pObj, uint16_t Reg, uint16_t Value)
{
  if ((IMX219_WriteReg8(pObj, Reg, (uint8_t)(Value >> 8)) != IMX219_OK) ||
      (IMX219_WriteReg8(pObj, (uint16_t)(Reg + 1U), (uint8_t)(Value & 0xffU)) != IMX219_OK))
  {
    return IMX219_ERROR;
  }

  return IMX219_OK;
}

static int32_t IMX219_ReadReg16(IMX219_Object_t *pObj, uint16_t Reg, uint16_t *Value)
{
  uint8_t addr[2];
  uint8_t data[2];

  if ((pObj == NULL) || (pObj->I2cHandle == NULL) || (Value == NULL))
  {
    return IMX219_ERROR;
  }

  addr[0] = (uint8_t)(Reg >> 8);
  addr[1] = (uint8_t)(Reg & 0xffU);

  if (HAL_I2C_Master_Transmit(pObj->I2cHandle, (uint16_t)(pObj->Address << 1), addr, sizeof(addr),
                              HAL_MAX_DELAY) != HAL_OK)
  {
    return IMX219_ERROR;
  }

  if (HAL_I2C_Master_Receive(pObj->I2cHandle, (uint16_t)(pObj->Address << 1), data, sizeof(data),
                             HAL_MAX_DELAY) != HAL_OK)
  {
    return IMX219_ERROR;
  }

  *Value = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
  return IMX219_OK;
}

static int32_t IMX219_WriteTable(IMX219_Object_t *pObj, const IMX219_Reg_t *Regs, uint32_t Count)
{
  uint32_t i;

  for (i = 0U; i < Count; i++)
  {
    if (IMX219_WriteReg8(pObj, Regs[i].Addr, Regs[i].Val) != IMX219_OK)
    {
      return IMX219_ERROR;
    }
  }

  return IMX219_OK;
}

static const IMX219_Mode_t *IMX219_FindMode(uint32_t Resolution)
{
  uint32_t i;

  for (i = 0U; i < (sizeof(IMX219_Modes) / sizeof(IMX219_Modes[0])); i++)
  {
    if (IMX219_Modes[i].Resolution == Resolution)
    {
      return &IMX219_Modes[i];
    }
  }

  return NULL;
}
