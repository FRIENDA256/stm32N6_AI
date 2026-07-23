/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    psram_init.c
  * @brief   APS256XXN PSRAM memory-mapped initialization for the FSBL.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "psram_init.h"

#include "xspi.h"

#define FSBL_PSRAM_READ_CMD               0x00U
#define FSBL_PSRAM_WRITE_CMD              0x80U
#define FSBL_PSRAM_READ_REG_CMD           0x40U
#define FSBL_PSRAM_WRITE_REG_CMD          0xC0U

#define FSBL_PSRAM_MR0                    0x00000000U
#define FSBL_PSRAM_MR4                    0x00000004U
#define FSBL_PSRAM_MR8                    0x00000008U

#define FSBL_PSRAM_DUMMY_READ             6U
#define FSBL_PSRAM_DUMMY_WRITE            6U
#define FSBL_PSRAM_CLOCK_PRESCALER        3U
#define FSBL_PSRAM_MM_DATA_LENGTH         10240U
#define FSBL_PSRAM_TEST_OFFSET            0x00010000UL

#define FSBL_PSRAM_STEP_RESET             1U
#define FSBL_PSRAM_STEP_MODE_REGISTERS    2U
#define FSBL_PSRAM_STEP_INDIRECT_TEST     3U
#define FSBL_PSRAM_STEP_MMAP_CONFIG       4U
#define FSBL_PSRAM_STEP_MMAP_ENABLE       5U
#define FSBL_PSRAM_STEP_MMAP_TEST         6U
#define FSBL_PSRAM_STEP_DONE              7U

#define FSBL_PSRAM_ERROR_NONE             0U
#define FSBL_PSRAM_ERROR_HAL              1U
#define FSBL_PSRAM_ERROR_REGISTER_VERIFY  2U
#define FSBL_PSRAM_ERROR_INDIRECT_VERIFY  3U
#define FSBL_PSRAM_ERROR_MMAP_VERIFY      4U

#define FSBL_PSRAM_STATUS_RECORD \
  ((volatile FSBL_PSRAM_StatusRecord_t *)FSBL_PSRAM_STATUS_ADDR)

static HAL_StatusTypeDef FSBL_PSRAM_WriteRegister(uint32_t address,
                                                  const uint8_t value[2]);
static HAL_StatusTypeDef FSBL_PSRAM_ReadRegister(uint32_t address,
                                                 uint8_t value[2],
                                                 uint32_t latency);
static HAL_StatusTypeDef FSBL_PSRAM_WriteData(uint32_t address,
                                              const uint8_t *data,
                                              uint32_t length);
static HAL_StatusTypeDef FSBL_PSRAM_ReadData(uint32_t address,
                                             uint8_t *data,
                                             uint32_t length);
static HAL_StatusTypeDef FSBL_PSRAM_ConfigureRegisters(void);
static HAL_StatusTypeDef FSBL_PSRAM_IndirectTest(void);
static HAL_StatusTypeDef FSBL_PSRAM_EnableMemoryMapped(void);
static HAL_StatusTypeDef FSBL_PSRAM_MemoryMappedTest(void);
static void FSBL_PSRAM_ConfigureMce(void);
static void FSBL_PSRAM_ResetStatus(void);
static void FSBL_PSRAM_SetStep(uint32_t step);
static void FSBL_PSRAM_SetFailed(uint32_t error_code);

HAL_StatusTypeDef FSBL_PSRAM_InitMemoryMapped(void)
{
  FSBL_PSRAM_ResetStatus();

  if (FSBL_PSRAM_ConfigureRegisters() != HAL_OK)
  {
    return HAL_ERROR;
  }
  if (FSBL_PSRAM_IndirectTest() != HAL_OK)
  {
    return HAL_ERROR;
  }
  if (FSBL_PSRAM_EnableMemoryMapped() != HAL_OK)
  {
    return HAL_ERROR;
  }
  if (FSBL_PSRAM_MemoryMappedTest() != HAL_OK)
  {
    return HAL_ERROR;
  }

  FSBL_PSRAM_SetStep(FSBL_PSRAM_STEP_DONE);
  FSBL_PSRAM_STATUS_RECORD->status = FSBL_PSRAM_STATUS_READY;
  FSBL_PSRAM_STATUS_RECORD->error_code = FSBL_PSRAM_ERROR_NONE;
  FSBL_PSRAM_STATUS_RECORD->hal_error = HAL_XSPI_GetError(&hxspi1);
  return HAL_OK;
}

static HAL_StatusTypeDef FSBL_PSRAM_WriteRegister(uint32_t address,
                                                  const uint8_t value[2])
{
  XSPI_RegularCmdTypeDef command = {0};

  command.OperationType = HAL_XSPI_OPTYPE_COMMON_CFG;
  command.InstructionMode = HAL_XSPI_INSTRUCTION_8_LINES;
  command.InstructionWidth = HAL_XSPI_INSTRUCTION_8_BITS;
  command.InstructionDTRMode = HAL_XSPI_INSTRUCTION_DTR_DISABLE;
  command.Instruction = FSBL_PSRAM_WRITE_REG_CMD;
  command.AddressMode = HAL_XSPI_ADDRESS_8_LINES;
  command.AddressWidth = HAL_XSPI_ADDRESS_32_BITS;
  command.AddressDTRMode = HAL_XSPI_ADDRESS_DTR_ENABLE;
  command.Address = address;
  command.AlternateBytesMode = HAL_XSPI_ALT_BYTES_NONE;
  command.DataMode = HAL_XSPI_DATA_8_LINES;
  command.DataDTRMode = HAL_XSPI_DATA_DTR_ENABLE;
  command.DataLength = 2U;
  command.DummyCycles = 0U;
  command.DQSMode = HAL_XSPI_DQS_DISABLE;

  if ((HAL_XSPI_Command(&hxspi1, &command, HAL_XSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) ||
      (HAL_XSPI_Transmit(&hxspi1,
                         (uint8_t *)value,
                         HAL_XSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK))
  {
    FSBL_PSRAM_SetFailed(FSBL_PSRAM_ERROR_HAL);
    return HAL_ERROR;
  }
  return HAL_OK;
}

static HAL_StatusTypeDef FSBL_PSRAM_ReadRegister(uint32_t address,
                                                 uint8_t value[2],
                                                 uint32_t latency)
{
  XSPI_RegularCmdTypeDef command = {0};

  command.OperationType = HAL_XSPI_OPTYPE_COMMON_CFG;
  command.InstructionMode = HAL_XSPI_INSTRUCTION_8_LINES;
  command.InstructionWidth = HAL_XSPI_INSTRUCTION_8_BITS;
  command.InstructionDTRMode = HAL_XSPI_INSTRUCTION_DTR_DISABLE;
  command.Instruction = FSBL_PSRAM_READ_REG_CMD;
  command.AddressMode = HAL_XSPI_ADDRESS_8_LINES;
  command.AddressWidth = HAL_XSPI_ADDRESS_32_BITS;
  command.AddressDTRMode = HAL_XSPI_ADDRESS_DTR_ENABLE;
  command.Address = address;
  command.AlternateBytesMode = HAL_XSPI_ALT_BYTES_NONE;
  command.DataMode = HAL_XSPI_DATA_8_LINES;
  command.DataDTRMode = HAL_XSPI_DATA_DTR_ENABLE;
  command.DataLength = 2U;
  command.DummyCycles = latency - 1U;
  command.DQSMode = HAL_XSPI_DQS_ENABLE;

  if ((HAL_XSPI_Command(&hxspi1, &command, HAL_XSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) ||
      (HAL_XSPI_Receive(&hxspi1,
                        value,
                        HAL_XSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK))
  {
    FSBL_PSRAM_SetFailed(FSBL_PSRAM_ERROR_HAL);
    return HAL_ERROR;
  }
  return HAL_OK;
}

static HAL_StatusTypeDef FSBL_PSRAM_WriteData(uint32_t address,
                                              const uint8_t *data,
                                              uint32_t length)
{
  XSPI_RegularCmdTypeDef command = {0};

  command.OperationType = HAL_XSPI_OPTYPE_COMMON_CFG;
  command.InstructionMode = HAL_XSPI_INSTRUCTION_8_LINES;
  command.InstructionWidth = HAL_XSPI_INSTRUCTION_8_BITS;
  command.InstructionDTRMode = HAL_XSPI_INSTRUCTION_DTR_DISABLE;
  command.Instruction = FSBL_PSRAM_WRITE_CMD;
  command.AddressMode = HAL_XSPI_ADDRESS_8_LINES;
  command.AddressWidth = HAL_XSPI_ADDRESS_32_BITS;
  command.AddressDTRMode = HAL_XSPI_ADDRESS_DTR_ENABLE;
  command.Address = address;
  command.AlternateBytesMode = HAL_XSPI_ALT_BYTES_NONE;
  command.DataMode = HAL_XSPI_DATA_16_LINES;
  command.DataDTRMode = HAL_XSPI_DATA_DTR_ENABLE;
  command.DataLength = length;
  command.DummyCycles = FSBL_PSRAM_DUMMY_WRITE;
  command.DQSMode = HAL_XSPI_DQS_ENABLE;

  if ((HAL_XSPI_Command(&hxspi1, &command, HAL_XSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) ||
      (HAL_XSPI_Transmit(&hxspi1,
                         (uint8_t *)data,
                         HAL_XSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK))
  {
    FSBL_PSRAM_SetFailed(FSBL_PSRAM_ERROR_HAL);
    return HAL_ERROR;
  }
  return HAL_OK;
}

static HAL_StatusTypeDef FSBL_PSRAM_ReadData(uint32_t address,
                                             uint8_t *data,
                                             uint32_t length)
{
  XSPI_RegularCmdTypeDef command = {0};

  command.OperationType = HAL_XSPI_OPTYPE_COMMON_CFG;
  command.InstructionMode = HAL_XSPI_INSTRUCTION_8_LINES;
  command.InstructionWidth = HAL_XSPI_INSTRUCTION_8_BITS;
  command.InstructionDTRMode = HAL_XSPI_INSTRUCTION_DTR_DISABLE;
  command.Instruction = FSBL_PSRAM_READ_CMD;
  command.AddressMode = HAL_XSPI_ADDRESS_8_LINES;
  command.AddressWidth = HAL_XSPI_ADDRESS_32_BITS;
  command.AddressDTRMode = HAL_XSPI_ADDRESS_DTR_ENABLE;
  command.Address = address;
  command.AlternateBytesMode = HAL_XSPI_ALT_BYTES_NONE;
  command.DataMode = HAL_XSPI_DATA_16_LINES;
  command.DataDTRMode = HAL_XSPI_DATA_DTR_ENABLE;
  command.DataLength = length;
  command.DummyCycles = FSBL_PSRAM_DUMMY_READ;
  command.DQSMode = HAL_XSPI_DQS_ENABLE;

  if ((HAL_XSPI_Command(&hxspi1, &command, HAL_XSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) ||
      (HAL_XSPI_Receive(&hxspi1,
                        data,
                        HAL_XSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK))
  {
    FSBL_PSRAM_SetFailed(FSBL_PSRAM_ERROR_HAL);
    return HAL_ERROR;
  }
  return HAL_OK;
}

static HAL_StatusTypeDef FSBL_PSRAM_ConfigureRegisters(void)
{
  const uint8_t mr0_write[2] = {0x30U, 0x8DU};
  const uint8_t mr4_write[2] = {0x20U, 0xF0U};
  const uint8_t mr8_write[2] = {0x4BU, 0x08U};
  uint8_t readback[2] = {0U};

  FSBL_PSRAM_SetStep(FSBL_PSRAM_STEP_MODE_REGISTERS);
  if (HAL_XSPI_SetClockPrescaler(&hxspi1, FSBL_PSRAM_CLOCK_PRESCALER) != HAL_OK)
  {
    FSBL_PSRAM_SetFailed(FSBL_PSRAM_ERROR_HAL);
    return HAL_ERROR;
  }

  if ((FSBL_PSRAM_WriteRegister(FSBL_PSRAM_MR0, mr0_write) != HAL_OK) ||
      (FSBL_PSRAM_ReadRegister(FSBL_PSRAM_MR0, readback, 6U) != HAL_OK) ||
      (readback[0] != mr0_write[0]))
  {
    FSBL_PSRAM_SetFailed(FSBL_PSRAM_ERROR_REGISTER_VERIFY);
    return HAL_ERROR;
  }
  if ((FSBL_PSRAM_WriteRegister(FSBL_PSRAM_MR4, mr4_write) != HAL_OK) ||
      (FSBL_PSRAM_ReadRegister(FSBL_PSRAM_MR4, readback, 6U) != HAL_OK) ||
      (readback[0] != mr4_write[0]))
  {
    FSBL_PSRAM_SetFailed(FSBL_PSRAM_ERROR_REGISTER_VERIFY);
    return HAL_ERROR;
  }
  if ((FSBL_PSRAM_WriteRegister(FSBL_PSRAM_MR8, mr8_write) != HAL_OK) ||
      (FSBL_PSRAM_ReadRegister(FSBL_PSRAM_MR8, readback, 6U) != HAL_OK) ||
      (readback[0] != mr8_write[0]))
  {
    FSBL_PSRAM_SetFailed(FSBL_PSRAM_ERROR_REGISTER_VERIFY);
    return HAL_ERROR;
  }
  return HAL_OK;
}

static HAL_StatusTypeDef FSBL_PSRAM_IndirectTest(void)
{
  const uint8_t expected[16] =
  {
    0xA5U, 0x5AU, 0x12U, 0x34U, 0xC3U, 0x3CU, 0x69U, 0x96U,
    0x00U, 0xFFU, 0x55U, 0xAAU, 0x78U, 0x87U, 0x1BU, 0xB1U
  };
  uint8_t actual[16] = {0U};

  FSBL_PSRAM_SetStep(FSBL_PSRAM_STEP_INDIRECT_TEST);
  if ((FSBL_PSRAM_WriteData(FSBL_PSRAM_TEST_OFFSET,
                            expected,
                            sizeof(expected)) != HAL_OK) ||
      (FSBL_PSRAM_ReadData(FSBL_PSRAM_TEST_OFFSET,
                           actual,
                           sizeof(actual)) != HAL_OK))
  {
    return HAL_ERROR;
  }
  for (uint32_t index = 0U; index < sizeof(expected); index++)
  {
    if (actual[index] != expected[index])
    {
      FSBL_PSRAM_STATUS_RECORD->test_offset = FSBL_PSRAM_TEST_OFFSET + index;
      FSBL_PSRAM_STATUS_RECORD->test_expected = expected[index];
      FSBL_PSRAM_STATUS_RECORD->test_actual = actual[index];
      FSBL_PSRAM_SetFailed(FSBL_PSRAM_ERROR_INDIRECT_VERIFY);
      return HAL_ERROR;
    }
  }
  return HAL_OK;
}

static HAL_StatusTypeDef FSBL_PSRAM_EnableMemoryMapped(void)
{
  XSPI_RegularCmdTypeDef command = {0};
  XSPI_MemoryMappedTypeDef mapped = {0};

  FSBL_PSRAM_SetStep(FSBL_PSRAM_STEP_MMAP_CONFIG);
  command.OperationType = HAL_XSPI_OPTYPE_WRITE_CFG;
  command.InstructionMode = HAL_XSPI_INSTRUCTION_8_LINES;
  command.InstructionWidth = HAL_XSPI_INSTRUCTION_8_BITS;
  command.InstructionDTRMode = HAL_XSPI_INSTRUCTION_DTR_DISABLE;
  command.Instruction = FSBL_PSRAM_WRITE_CMD;
  command.AddressMode = HAL_XSPI_ADDRESS_8_LINES;
  command.AddressWidth = HAL_XSPI_ADDRESS_32_BITS;
  command.AddressDTRMode = HAL_XSPI_ADDRESS_DTR_ENABLE;
  command.AlternateBytesMode = HAL_XSPI_ALT_BYTES_NONE;
  command.DataMode = HAL_XSPI_DATA_16_LINES;
  command.DataDTRMode = HAL_XSPI_DATA_DTR_ENABLE;
  command.DataLength = FSBL_PSRAM_MM_DATA_LENGTH;
  command.DummyCycles = FSBL_PSRAM_DUMMY_WRITE;
  command.DQSMode = HAL_XSPI_DQS_ENABLE;
  if (HAL_XSPI_Command(&hxspi1, &command, HAL_XSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
  {
    FSBL_PSRAM_SetFailed(FSBL_PSRAM_ERROR_HAL);
    return HAL_ERROR;
  }

  command.OperationType = HAL_XSPI_OPTYPE_READ_CFG;
  command.Instruction = FSBL_PSRAM_READ_CMD;
  command.DummyCycles = FSBL_PSRAM_DUMMY_READ;
  if (HAL_XSPI_Command(&hxspi1, &command, HAL_XSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
  {
    FSBL_PSRAM_SetFailed(FSBL_PSRAM_ERROR_HAL);
    return HAL_ERROR;
  }

  mapped.TimeOutActivation = HAL_XSPI_TIMEOUT_COUNTER_ENABLE;
  mapped.TimeoutPeriodClock = 0x34U;
  FSBL_PSRAM_SetStep(FSBL_PSRAM_STEP_MMAP_ENABLE);
  if (HAL_XSPI_MemoryMapped(&hxspi1, &mapped) != HAL_OK)
  {
    FSBL_PSRAM_SetFailed(FSBL_PSRAM_ERROR_HAL);
    return HAL_ERROR;
  }

  FSBL_PSRAM_ConfigureMce();
  return HAL_OK;
}

static HAL_StatusTypeDef FSBL_PSRAM_MemoryMappedTest(void)
{
  volatile uint32_t *test =
    (volatile uint32_t *)(FSBL_PSRAM_BASE_ADDR + FSBL_PSRAM_TEST_OFFSET);
  const uint32_t expected[4] =
  {
    0x11223344U, 0xA5A55A5AU, 0x55AA00FFU, 0xC33C9669U
  };

  FSBL_PSRAM_SetStep(FSBL_PSRAM_STEP_MMAP_TEST);
  for (uint32_t index = 0U; index < 4U; index++)
  {
    test[index] = expected[index];
  }
  __DSB();
  for (uint32_t index = 0U; index < 4U; index++)
  {
    uint32_t actual = test[index];

    if (actual != expected[index])
    {
      FSBL_PSRAM_STATUS_RECORD->test_offset =
        FSBL_PSRAM_TEST_OFFSET + (index * sizeof(uint32_t));
      FSBL_PSRAM_STATUS_RECORD->test_expected = expected[index];
      FSBL_PSRAM_STATUS_RECORD->test_actual = actual;
      FSBL_PSRAM_SetFailed(FSBL_PSRAM_ERROR_MMAP_VERIFY);
      return HAL_ERROR;
    }
  }
  return HAL_OK;
}

static void FSBL_PSRAM_ConfigureMce(void)
{
  __HAL_RCC_MCE1_CLK_ENABLE();
  CLEAR_BIT(MCE1_REGION1->REGCR, MCE_REGCR_BREN);
  MODIFY_REG(MCE1_REGION1->REGCR, MCE_REGCR_ENC, 0U);
  MCE1_REGION1->SADDR = FSBL_PSRAM_BASE_ADDR;
  MCE1_REGION1->EADDR = FSBL_PSRAM_BASE_ADDR + FSBL_PSRAM_SIZE_BYTES - 1U;
  SET_BIT(MCE1_REGION1->REGCR, MCE_REGCR_BREN);
}

static void FSBL_PSRAM_ResetStatus(void)
{
  volatile uint32_t *words = (volatile uint32_t *)FSBL_PSRAM_STATUS_RECORD;

  for (uint32_t index = 0U;
       index < (sizeof(FSBL_PSRAM_StatusRecord_t) / sizeof(uint32_t));
       index++)
  {
    words[index] = 0U;
  }
  FSBL_PSRAM_STATUS_RECORD->magic = FSBL_PSRAM_STATUS_MAGIC;
  FSBL_PSRAM_STATUS_RECORD->version = FSBL_PSRAM_STATUS_VERSION;
  FSBL_PSRAM_STATUS_RECORD->status = FSBL_PSRAM_STATUS_NOT_READY;
  FSBL_PSRAM_STATUS_RECORD->step = FSBL_PSRAM_STEP_RESET;
  FSBL_PSRAM_STATUS_RECORD->base_address = FSBL_PSRAM_BASE_ADDR;
  FSBL_PSRAM_STATUS_RECORD->size_bytes = FSBL_PSRAM_SIZE_BYTES;
}

static void FSBL_PSRAM_SetStep(uint32_t step)
{
  FSBL_PSRAM_STATUS_RECORD->step = step;
}

static void FSBL_PSRAM_SetFailed(uint32_t error_code)
{
  FSBL_PSRAM_STATUS_RECORD->status = FSBL_PSRAM_STATUS_FAILED;
  FSBL_PSRAM_STATUS_RECORD->error_code = error_code;
  FSBL_PSRAM_STATUS_RECORD->hal_error = HAL_XSPI_GetError(&hxspi1);
}
