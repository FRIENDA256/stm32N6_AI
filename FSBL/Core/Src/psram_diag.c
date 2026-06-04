/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    psram_diag.c
  * @brief   APS256XXN PSRAM bring-up helpers for FSBL diagnostics.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "psram_diag.h"
#include "xspi.h"

#define FSBL_PSRAM_BASE_ADDR              XSPI1_BASE

#define FSBL_PSRAM_READ_CMD               0x00U
#define FSBL_PSRAM_WRITE_CMD              0x80U
#define FSBL_PSRAM_READ_REG_CMD           0x40U
#define FSBL_PSRAM_WRITE_REG_CMD          0xC0U

#define FSBL_PSRAM_MR0                    0x00000000U
#define FSBL_PSRAM_MR4                    0x00000004U
#define FSBL_PSRAM_MR8                    0x00000008U

#define FSBL_PSRAM_DUMMY_READ             6U
#define FSBL_PSRAM_DUMMY_WRITE            6U
#define FSBL_PSRAM_MM_DATA_LENGTH         10240U
#define FSBL_PSRAM_MCE_BASE_ADDR          0x90000000UL
#define FSBL_PSRAM_MCE_END_ADDR           0x91FFFFFFUL

#define FSBL_PSRAM_FAIL_REG               1U
#define FSBL_PSRAM_FAIL_MMAP              2U
#define FSBL_PSRAM_FAIL_TEST              3U

#define FSBL_PSRAM_STEP_START             1U
#define FSBL_PSRAM_STEP_WRITE_MR0         2U
#define FSBL_PSRAM_STEP_READ_MR0          3U
#define FSBL_PSRAM_STEP_WRITE_MR4         4U
#define FSBL_PSRAM_STEP_READ_MR4          5U
#define FSBL_PSRAM_STEP_WRITE_MR8         6U
#define FSBL_PSRAM_STEP_READ_MR8          7U
#define FSBL_PSRAM_STEP_MMAP_WRITE_CFG    8U
#define FSBL_PSRAM_STEP_MMAP_READ_CFG     9U
#define FSBL_PSRAM_STEP_MMAP_ENABLE       10U
#define FSBL_PSRAM_STEP_TEST_PATTERN1     11U
#define FSBL_PSRAM_STEP_TEST_PATTERN2     12U
#define FSBL_PSRAM_STEP_TEST_BYTE_PREPARE 13U
#define FSBL_PSRAM_STEP_INDIRECT_WRITE    14U
#define FSBL_PSRAM_STEP_INDIRECT_READ     15U
#define FSBL_PSRAM_STEP_DONE              16U
#define FSBL_PSRAM_STEP_TEST_BYTE_READ    17U

#define FSBL_PSRAM_ERROR_NONE             0U
#define FSBL_PSRAM_ERROR_HAL              1U
#define FSBL_PSRAM_ERROR_VERIFY_MR0       2U
#define FSBL_PSRAM_ERROR_VERIFY_MR4       3U
#define FSBL_PSRAM_ERROR_VERIFY_MR8       4U
#define FSBL_PSRAM_ERROR_VERIFY_MEM       5U
#define FSBL_PSRAM_ERROR_VERIFY_INDIRECT  6U

#define FSBL_PSRAM_DIAG_RECORD            ((volatile FSBL_PSRAM_DiagRecord_t *)FSBL_PSRAM_DIAG_RECORD_ADDR)

static HAL_StatusTypeDef APS256_WriteReg(XSPI_HandleTypeDef *ctx, uint32_t address, uint8_t *value);
static HAL_StatusTypeDef APS256_ReadReg(XSPI_HandleTypeDef *ctx, uint32_t address, uint8_t *value, uint32_t latency_code);
static HAL_StatusTypeDef APS256_WriteData(XSPI_HandleTypeDef *ctx, uint32_t address, uint8_t *data, uint32_t length);
static HAL_StatusTypeDef APS256_ReadData(XSPI_HandleTypeDef *ctx, uint32_t address, uint8_t *data, uint32_t length);
static HAL_StatusTypeDef FSBL_PSRAM_ConfigureMemory(void);
static HAL_StatusTypeDef FSBL_PSRAM_IndirectTest(void);
static HAL_StatusTypeDef FSBL_PSRAM_EnterMemoryMapped(void);
static void FSBL_PSRAM_ConfigMcePlainRegion(void);
#if (FSBL_PSRAM_DIAG_LEVEL >= 3U)
static HAL_StatusTypeDef FSBL_PSRAM_ByteProbe(void);
#endif
static void FSBL_PSRAM_DiagReset(void);
static void FSBL_PSRAM_DiagSetStep(uint32_t step);
static void FSBL_PSRAM_DiagSetFail(uint32_t step, uint32_t error_code);
static void FSBL_PSRAM_DiagSetOk(void);
static void FSBL_PSRAM_RecordXspiBeforeMmap(void);
static void FSBL_PSRAM_RecordXspiAfterMmap(void);
static void FSBL_PSRAM_RecordAccessPath(void);
static void FSBL_PSRAM_DiagLedInit(void);
#if FSBL_PSRAM_BLOCK_ON_FAIL
static void FSBL_PSRAM_DiagLedBlink(uint32_t count);
#endif
static void FSBL_PSRAM_DiagLedPulse(uint32_t count, uint32_t on_ms, uint32_t off_ms);
static void FSBL_PSRAM_DiagLedPulseOk(void);

HAL_StatusTypeDef FSBL_PSRAM_InitMemoryMapped(void)
{
  if (FSBL_PSRAM_ConfigureMemory() != HAL_OK)
  {
    return HAL_ERROR;
  }

  return FSBL_PSRAM_EnterMemoryMapped();
}

static HAL_StatusTypeDef FSBL_PSRAM_EnterMemoryMapped(void)
{
  XSPI_RegularCmdTypeDef command = {0};
  XSPI_MemoryMappedTypeDef mem_mapped_cfg = {0};

  if (HAL_XSPI_SetClockPrescaler(&hxspi1, 0U) != HAL_OK)
  {
    FSBL_PSRAM_DiagSetFail(FSBL_PSRAM_STEP_MMAP_WRITE_CFG, FSBL_PSRAM_ERROR_HAL);
    return HAL_ERROR;
  }

  command.OperationType      = HAL_XSPI_OPTYPE_WRITE_CFG;
  command.InstructionMode    = HAL_XSPI_INSTRUCTION_8_LINES;
  command.InstructionWidth   = HAL_XSPI_INSTRUCTION_8_BITS;
  command.InstructionDTRMode = HAL_XSPI_INSTRUCTION_DTR_DISABLE;
  command.Instruction        = FSBL_PSRAM_WRITE_CMD;
  command.AddressMode        = HAL_XSPI_ADDRESS_8_LINES;
  command.AddressWidth       = HAL_XSPI_ADDRESS_32_BITS;
  command.AddressDTRMode     = HAL_XSPI_ADDRESS_DTR_ENABLE;
  command.Address            = 0U;
  command.AlternateBytesMode = HAL_XSPI_ALT_BYTES_NONE;
  command.DataMode           = HAL_XSPI_DATA_16_LINES;
  command.DataDTRMode        = HAL_XSPI_DATA_DTR_ENABLE;
  command.DataLength         = FSBL_PSRAM_MM_DATA_LENGTH;
  command.DummyCycles        = FSBL_PSRAM_DUMMY_WRITE;
  command.DQSMode            = HAL_XSPI_DQS_ENABLE;

  FSBL_PSRAM_DiagSetStep(FSBL_PSRAM_STEP_MMAP_WRITE_CFG);
  if (HAL_XSPI_Command(&hxspi1, &command, HAL_XSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
  {
    FSBL_PSRAM_DiagSetFail(FSBL_PSRAM_STEP_MMAP_WRITE_CFG, FSBL_PSRAM_ERROR_HAL);
    return HAL_ERROR;
  }

  command.OperationType = HAL_XSPI_OPTYPE_READ_CFG;
  command.Instruction   = FSBL_PSRAM_READ_CMD;
  command.DummyCycles   = FSBL_PSRAM_DUMMY_READ;
  command.DQSMode       = HAL_XSPI_DQS_ENABLE;

  FSBL_PSRAM_DiagSetStep(FSBL_PSRAM_STEP_MMAP_READ_CFG);
  if (HAL_XSPI_Command(&hxspi1, &command, HAL_XSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
  {
    FSBL_PSRAM_DiagSetFail(FSBL_PSRAM_STEP_MMAP_READ_CFG, FSBL_PSRAM_ERROR_HAL);
    return HAL_ERROR;
  }

  mem_mapped_cfg.TimeOutActivation = HAL_XSPI_TIMEOUT_COUNTER_ENABLE;
  mem_mapped_cfg.TimeoutPeriodClock = 0x34U;

  FSBL_PSRAM_RecordXspiBeforeMmap();
  FSBL_PSRAM_DiagSetStep(FSBL_PSRAM_STEP_MMAP_ENABLE);
  if (HAL_XSPI_MemoryMapped(&hxspi1, &mem_mapped_cfg) != HAL_OK)
  {
    FSBL_PSRAM_DiagSetFail(FSBL_PSRAM_STEP_MMAP_ENABLE, FSBL_PSRAM_ERROR_HAL);
    return HAL_ERROR;
  }
  FSBL_PSRAM_RecordXspiAfterMmap();
  FSBL_PSRAM_ConfigMcePlainRegion();
  FSBL_PSRAM_RecordAccessPath();

  return HAL_OK;
}

HAL_StatusTypeDef FSBL_PSRAM_Test(uint32_t words)
{
  volatile uint32_t *ram = (volatile uint32_t *)FSBL_PSRAM_BASE_ADDR;
  uint32_t i;
  uint32_t expected;

  FSBL_PSRAM_DIAG_RECORD->test_words = words;
  FSBL_PSRAM_DiagSetStep(FSBL_PSRAM_STEP_TEST_PATTERN1);
  for (i = 0U; i < words; i++)
  {
    ram[i] = 0xA5A50000U ^ (i * 0x10203041U);
  }

  for (i = 0U; i < words; i++)
  {
    expected = 0xA5A50000U ^ (i * 0x10203041U);
    if (ram[i] != expected)
    {
      FSBL_PSRAM_DIAG_RECORD->fail_index = i;
      FSBL_PSRAM_DIAG_RECORD->expected = expected;
      FSBL_PSRAM_DIAG_RECORD->actual = ram[i];
      FSBL_PSRAM_DiagSetFail(FSBL_PSRAM_STEP_TEST_PATTERN1, FSBL_PSRAM_ERROR_VERIFY_MEM);
      return HAL_ERROR;
    }
  }

  FSBL_PSRAM_DiagSetStep(FSBL_PSRAM_STEP_TEST_PATTERN2);
  for (i = 0U; i < words; i++)
  {
    ram[i] = 0x5A5A0000U ^ (i * 0x01010101U);
  }

  for (i = 0U; i < words; i++)
  {
    expected = 0x5A5A0000U ^ (i * 0x01010101U);
    if (ram[i] != expected)
    {
      FSBL_PSRAM_DIAG_RECORD->fail_index = i;
      FSBL_PSRAM_DIAG_RECORD->expected = expected;
      FSBL_PSRAM_DIAG_RECORD->actual = ram[i];
      FSBL_PSRAM_DiagSetFail(FSBL_PSRAM_STEP_TEST_PATTERN2, FSBL_PSRAM_ERROR_VERIFY_MEM);
      return HAL_ERROR;
    }
  }

  return HAL_OK;
}

HAL_StatusTypeDef FSBL_PSRAM_InitAndTest(void)
{
  FSBL_PSRAM_DiagReset();
  FSBL_PSRAM_DiagLedInit();
  FSBL_PSRAM_DiagLedPulse(1U, 80U, 80U);

  if (FSBL_PSRAM_ConfigureMemory() != HAL_OK)
  {
    FSBL_PSRAM_DiagLedPulse(FSBL_PSRAM_FAIL_REG, 260U, 160U);
#if FSBL_PSRAM_BLOCK_ON_FAIL
    FSBL_PSRAM_DiagLedBlink(FSBL_PSRAM_FAIL_REG);
#endif
    return HAL_ERROR;
  }

#if (FSBL_PSRAM_DIAG_LEVEL <= 1U)
  FSBL_PSRAM_DiagSetOk();
  FSBL_PSRAM_DiagLedPulseOk();
  return HAL_OK;
#endif

  if (FSBL_PSRAM_IndirectTest() != HAL_OK)
  {
    FSBL_PSRAM_DiagLedPulse(FSBL_PSRAM_FAIL_TEST, 260U, 160U);
#if FSBL_PSRAM_BLOCK_ON_FAIL
    FSBL_PSRAM_DiagLedBlink(FSBL_PSRAM_FAIL_TEST);
#endif
    return HAL_ERROR;
  }

  if (FSBL_PSRAM_EnterMemoryMapped() != HAL_OK)
  {
    FSBL_PSRAM_DiagLedPulse(FSBL_PSRAM_FAIL_MMAP, 260U, 160U);
#if FSBL_PSRAM_BLOCK_ON_FAIL
    FSBL_PSRAM_DiagLedBlink(FSBL_PSRAM_FAIL_MMAP);
#endif
    return HAL_ERROR;
  }

#if (FSBL_PSRAM_DIAG_LEVEL <= 2U)
  FSBL_PSRAM_DiagSetOk();
  FSBL_PSRAM_DiagLedPulseOk();
  return HAL_OK;
#endif

#if (FSBL_PSRAM_DIAG_LEVEL >= 3U)
  if (FSBL_PSRAM_ByteProbe() != HAL_OK)
  {
    FSBL_PSRAM_DiagLedPulse(FSBL_PSRAM_FAIL_TEST, 260U, 160U);
#if FSBL_PSRAM_BLOCK_ON_FAIL
    FSBL_PSRAM_DiagLedBlink(FSBL_PSRAM_FAIL_TEST);
#endif
    return HAL_ERROR;
  }
#endif

#if (FSBL_PSRAM_DIAG_LEVEL <= 3U)
  FSBL_PSRAM_DiagSetOk();
  FSBL_PSRAM_DiagLedPulseOk();
  return HAL_OK;
#endif

  if (FSBL_PSRAM_Test(FSBL_PSRAM_TEST_WORDS) != HAL_OK)
  {
    FSBL_PSRAM_DiagLedPulse(FSBL_PSRAM_FAIL_TEST, 260U, 160U);
#if FSBL_PSRAM_BLOCK_ON_FAIL
    FSBL_PSRAM_DiagLedBlink(FSBL_PSRAM_FAIL_TEST);
#endif
    return HAL_ERROR;
  }

  FSBL_PSRAM_DiagSetOk();
  FSBL_PSRAM_DiagLedPulseOk();
  return HAL_OK;
}

#if (FSBL_PSRAM_DIAG_LEVEL >= 3U)
static HAL_StatusTypeDef FSBL_PSRAM_ByteProbe(void)
{
  volatile uint8_t *ram = (volatile uint8_t *)(FSBL_PSRAM_BASE_ADDR + 0x100U);
  const uint8_t expected = 0xA5U;
  uint8_t actual;

  HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET);

  FSBL_PSRAM_DiagSetStep(FSBL_PSRAM_STEP_TEST_BYTE_PREPARE);
  FSBL_PSRAM_DIAG_RECORD->fail_index = 0x100U;
  FSBL_PSRAM_DIAG_RECORD->expected = expected;

  FSBL_PSRAM_DiagSetStep(FSBL_PSRAM_STEP_TEST_BYTE_READ);
  actual = ram[0];
  if (actual != expected)
  {
    FSBL_PSRAM_DIAG_RECORD->fail_index = 0U;
    FSBL_PSRAM_DIAG_RECORD->expected = expected;
    FSBL_PSRAM_DIAG_RECORD->actual = actual;
    FSBL_PSRAM_DiagSetFail(FSBL_PSRAM_STEP_TEST_BYTE_READ, FSBL_PSRAM_ERROR_VERIFY_MEM);
    return HAL_ERROR;
  }

  HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);

  return HAL_OK;
}
#endif

static HAL_StatusTypeDef APS256_WriteData(XSPI_HandleTypeDef *ctx, uint32_t address, uint8_t *data, uint32_t length)
{
  XSPI_RegularCmdTypeDef command = {0};

  command.OperationType      = HAL_XSPI_OPTYPE_COMMON_CFG;
  command.InstructionMode    = HAL_XSPI_INSTRUCTION_8_LINES;
  command.InstructionWidth   = HAL_XSPI_INSTRUCTION_8_BITS;
  command.InstructionDTRMode = HAL_XSPI_INSTRUCTION_DTR_DISABLE;
  command.Instruction        = FSBL_PSRAM_WRITE_CMD;
  command.AddressMode        = HAL_XSPI_ADDRESS_8_LINES;
  command.AddressWidth       = HAL_XSPI_ADDRESS_32_BITS;
  command.AddressDTRMode     = HAL_XSPI_ADDRESS_DTR_ENABLE;
  command.Address            = address;
  command.AlternateBytesMode = HAL_XSPI_ALT_BYTES_NONE;
  command.DataMode           = HAL_XSPI_DATA_16_LINES;
  command.DataDTRMode        = HAL_XSPI_DATA_DTR_ENABLE;
  command.DataLength         = length;
  command.DummyCycles        = FSBL_PSRAM_DUMMY_WRITE;
  command.DQSMode            = HAL_XSPI_DQS_ENABLE;

  if (HAL_XSPI_Command(ctx, &command, HAL_XSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
  {
    FSBL_PSRAM_DiagSetFail(FSBL_PSRAM_DIAG_RECORD->step, FSBL_PSRAM_ERROR_HAL);
    return HAL_ERROR;
  }

  if (HAL_XSPI_Transmit(ctx, data, HAL_XSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
  {
    FSBL_PSRAM_DiagSetFail(FSBL_PSRAM_DIAG_RECORD->step, FSBL_PSRAM_ERROR_HAL);
    return HAL_ERROR;
  }

  return HAL_OK;
}

static HAL_StatusTypeDef APS256_ReadData(XSPI_HandleTypeDef *ctx, uint32_t address, uint8_t *data, uint32_t length)
{
  XSPI_RegularCmdTypeDef command = {0};

  command.OperationType      = HAL_XSPI_OPTYPE_COMMON_CFG;
  command.InstructionMode    = HAL_XSPI_INSTRUCTION_8_LINES;
  command.InstructionWidth   = HAL_XSPI_INSTRUCTION_8_BITS;
  command.InstructionDTRMode = HAL_XSPI_INSTRUCTION_DTR_DISABLE;
  command.Instruction        = FSBL_PSRAM_READ_CMD;
  command.AddressMode        = HAL_XSPI_ADDRESS_8_LINES;
  command.AddressWidth       = HAL_XSPI_ADDRESS_32_BITS;
  command.AddressDTRMode     = HAL_XSPI_ADDRESS_DTR_ENABLE;
  command.Address            = address;
  command.AlternateBytesMode = HAL_XSPI_ALT_BYTES_NONE;
  command.DataMode           = HAL_XSPI_DATA_16_LINES;
  command.DataDTRMode        = HAL_XSPI_DATA_DTR_ENABLE;
  command.DataLength         = length;
  command.DummyCycles        = FSBL_PSRAM_DUMMY_READ;
  command.DQSMode            = HAL_XSPI_DQS_ENABLE;

  if (HAL_XSPI_Command(ctx, &command, HAL_XSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
  {
    FSBL_PSRAM_DiagSetFail(FSBL_PSRAM_DIAG_RECORD->step, FSBL_PSRAM_ERROR_HAL);
    return HAL_ERROR;
  }

  if (HAL_XSPI_Receive(ctx, data, HAL_XSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
  {
    FSBL_PSRAM_DiagSetFail(FSBL_PSRAM_DIAG_RECORD->step, FSBL_PSRAM_ERROR_HAL);
    return HAL_ERROR;
  }

  return HAL_OK;
}

static HAL_StatusTypeDef APS256_WriteReg(XSPI_HandleTypeDef *ctx, uint32_t address, uint8_t *value)
{
  XSPI_RegularCmdTypeDef command = {0};

  command.OperationType      = HAL_XSPI_OPTYPE_COMMON_CFG;
  command.InstructionMode    = HAL_XSPI_INSTRUCTION_8_LINES;
  command.InstructionWidth   = HAL_XSPI_INSTRUCTION_8_BITS;
  command.InstructionDTRMode = HAL_XSPI_INSTRUCTION_DTR_DISABLE;
  command.Instruction        = FSBL_PSRAM_WRITE_REG_CMD;
  command.AddressMode        = HAL_XSPI_ADDRESS_8_LINES;
  command.AddressWidth       = HAL_XSPI_ADDRESS_32_BITS;
  command.AddressDTRMode     = HAL_XSPI_ADDRESS_DTR_ENABLE;
  command.Address            = address;
  command.AlternateBytesMode = HAL_XSPI_ALT_BYTES_NONE;
  command.DataMode           = HAL_XSPI_DATA_8_LINES;
  command.DataDTRMode        = HAL_XSPI_DATA_DTR_ENABLE;
  command.DataLength         = 2U;
  command.DummyCycles        = 0U;
  command.DQSMode            = HAL_XSPI_DQS_DISABLE;

  if (HAL_XSPI_Command(ctx, &command, HAL_XSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
  {
    FSBL_PSRAM_DiagSetFail(FSBL_PSRAM_DIAG_RECORD->step, FSBL_PSRAM_ERROR_HAL);
    return HAL_ERROR;
  }

  if (HAL_XSPI_Transmit(ctx, value, HAL_XSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
  {
    FSBL_PSRAM_DiagSetFail(FSBL_PSRAM_DIAG_RECORD->step, FSBL_PSRAM_ERROR_HAL);
    return HAL_ERROR;
  }

  return HAL_OK;
}

static HAL_StatusTypeDef APS256_ReadReg(XSPI_HandleTypeDef *ctx, uint32_t address, uint8_t *value, uint32_t latency_code)
{
  XSPI_RegularCmdTypeDef command = {0};

  command.OperationType      = HAL_XSPI_OPTYPE_COMMON_CFG;
  command.InstructionMode    = HAL_XSPI_INSTRUCTION_8_LINES;
  command.InstructionWidth   = HAL_XSPI_INSTRUCTION_8_BITS;
  command.InstructionDTRMode = HAL_XSPI_INSTRUCTION_DTR_DISABLE;
  command.Instruction        = FSBL_PSRAM_READ_REG_CMD;
  command.AddressMode        = HAL_XSPI_ADDRESS_8_LINES;
  command.AddressWidth       = HAL_XSPI_ADDRESS_32_BITS;
  command.AddressDTRMode     = HAL_XSPI_ADDRESS_DTR_ENABLE;
  command.Address            = address;
  command.AlternateBytesMode = HAL_XSPI_ALT_BYTES_NONE;
  command.DataMode           = HAL_XSPI_DATA_8_LINES;
  command.DataDTRMode        = HAL_XSPI_DATA_DTR_ENABLE;
  command.DataLength         = 2U;
  command.DummyCycles        = latency_code - 1U;
  command.DQSMode            = HAL_XSPI_DQS_ENABLE;

  if (HAL_XSPI_Command(ctx, &command, HAL_XSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
  {
    FSBL_PSRAM_DiagSetFail(FSBL_PSRAM_DIAG_RECORD->step, FSBL_PSRAM_ERROR_HAL);
    return HAL_ERROR;
  }

  if (HAL_XSPI_Receive(ctx, value, HAL_XSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
  {
    FSBL_PSRAM_DiagSetFail(FSBL_PSRAM_DIAG_RECORD->step, FSBL_PSRAM_ERROR_HAL);
    return HAL_ERROR;
  }

  return HAL_OK;
}

static HAL_StatusTypeDef FSBL_PSRAM_IndirectTest(void)
{
  uint8_t tx[16] =
  {
    0xA5U, 0x5AU, 0x12U, 0x34U, 0xC3U, 0x3CU, 0x69U, 0x96U,
    0x00U, 0xFFU, 0x55U, 0xAAU, 0x78U, 0x87U, 0x1BU, 0xB1U
  };
  uint8_t rx[16] = {0U};
  uint32_t i;

  FSBL_PSRAM_DIAG_RECORD->indirect_len = sizeof(tx);
  for (i = 0U; i < sizeof(tx); i++)
  {
    FSBL_PSRAM_DIAG_RECORD->indirect_tx[i] = tx[i];
    FSBL_PSRAM_DIAG_RECORD->indirect_rx[i] = 0U;
  }

  FSBL_PSRAM_DiagSetStep(FSBL_PSRAM_STEP_INDIRECT_WRITE);
  if (APS256_WriteData(&hxspi1, 0x00000100U, tx, sizeof(tx)) != HAL_OK)
  {
    return HAL_ERROR;
  }

  FSBL_PSRAM_DiagSetStep(FSBL_PSRAM_STEP_INDIRECT_READ);
  if (APS256_ReadData(&hxspi1, 0x00000100U, rx, sizeof(rx)) != HAL_OK)
  {
    return HAL_ERROR;
  }

  for (i = 0U; i < sizeof(rx); i++)
  {
    FSBL_PSRAM_DIAG_RECORD->indirect_rx[i] = rx[i];
    if (rx[i] != tx[i])
    {
      FSBL_PSRAM_DIAG_RECORD->fail_index = i;
      FSBL_PSRAM_DIAG_RECORD->expected = tx[i];
      FSBL_PSRAM_DIAG_RECORD->actual = rx[i];
      FSBL_PSRAM_DiagSetFail(FSBL_PSRAM_STEP_INDIRECT_READ, FSBL_PSRAM_ERROR_VERIFY_INDIRECT);
      return HAL_ERROR;
    }
  }

  return HAL_OK;
}

static HAL_StatusTypeDef FSBL_PSRAM_ConfigureMemory(void)
{
  uint8_t reg_w_mr0[2] = {0x30U, 0x8DU};
  uint8_t reg_r_mr0[2] = {0U};
  uint8_t reg_w_mr4[2] = {0x20U, 0xF0U};
  uint8_t reg_r_mr4[2] = {0U};
  uint8_t reg_w_mr8[2] = {0x4BU, 0x08U};
  uint8_t reg_r_mr8[2] = {0U};
  uint32_t latency = 6U;

  FSBL_PSRAM_DIAG_RECORD->mr0_w[0] = reg_w_mr0[0];
  FSBL_PSRAM_DIAG_RECORD->mr0_w[1] = reg_w_mr0[1];
  FSBL_PSRAM_DIAG_RECORD->mr4_w[0] = reg_w_mr4[0];
  FSBL_PSRAM_DIAG_RECORD->mr4_w[1] = reg_w_mr4[1];
  FSBL_PSRAM_DIAG_RECORD->mr8_w[0] = reg_w_mr8[0];
  FSBL_PSRAM_DIAG_RECORD->mr8_w[1] = reg_w_mr8[1];

  FSBL_PSRAM_DiagSetStep(FSBL_PSRAM_STEP_WRITE_MR0);
  if (APS256_WriteReg(&hxspi1, FSBL_PSRAM_MR0, reg_w_mr0) != HAL_OK)
  {
    return HAL_ERROR;
  }

  FSBL_PSRAM_DiagSetStep(FSBL_PSRAM_STEP_READ_MR0);
  if (APS256_ReadReg(&hxspi1, FSBL_PSRAM_MR0, reg_r_mr0, latency) != HAL_OK)
  {
    return HAL_ERROR;
  }
  FSBL_PSRAM_DIAG_RECORD->mr0_r[0] = reg_r_mr0[0];
  FSBL_PSRAM_DIAG_RECORD->mr0_r[1] = reg_r_mr0[1];

  if (reg_r_mr0[0] != reg_w_mr0[0])
  {
    FSBL_PSRAM_DIAG_RECORD->expected = reg_w_mr0[0];
    FSBL_PSRAM_DIAG_RECORD->actual = reg_r_mr0[0];
    FSBL_PSRAM_DiagSetFail(FSBL_PSRAM_STEP_READ_MR0, FSBL_PSRAM_ERROR_VERIFY_MR0);
    return HAL_ERROR;
  }

  FSBL_PSRAM_DiagSetStep(FSBL_PSRAM_STEP_WRITE_MR4);
  if (APS256_WriteReg(&hxspi1, FSBL_PSRAM_MR4, reg_w_mr4) != HAL_OK)
  {
    return HAL_ERROR;
  }

  FSBL_PSRAM_DiagSetStep(FSBL_PSRAM_STEP_READ_MR4);
  if (APS256_ReadReg(&hxspi1, FSBL_PSRAM_MR4, reg_r_mr4, latency) != HAL_OK)
  {
    return HAL_ERROR;
  }
  FSBL_PSRAM_DIAG_RECORD->mr4_r[0] = reg_r_mr4[0];
  FSBL_PSRAM_DIAG_RECORD->mr4_r[1] = reg_r_mr4[1];

  if (reg_r_mr4[0] != reg_w_mr4[0])
  {
    FSBL_PSRAM_DIAG_RECORD->expected = reg_w_mr4[0];
    FSBL_PSRAM_DIAG_RECORD->actual = reg_r_mr4[0];
    FSBL_PSRAM_DiagSetFail(FSBL_PSRAM_STEP_READ_MR4, FSBL_PSRAM_ERROR_VERIFY_MR4);
    return HAL_ERROR;
  }

  FSBL_PSRAM_DiagSetStep(FSBL_PSRAM_STEP_WRITE_MR8);
  if (APS256_WriteReg(&hxspi1, FSBL_PSRAM_MR8, reg_w_mr8) != HAL_OK)
  {
    return HAL_ERROR;
  }

  FSBL_PSRAM_DiagSetStep(FSBL_PSRAM_STEP_READ_MR8);
  if (APS256_ReadReg(&hxspi1, FSBL_PSRAM_MR8, reg_r_mr8, latency) != HAL_OK)
  {
    return HAL_ERROR;
  }
  FSBL_PSRAM_DIAG_RECORD->mr8_r[0] = reg_r_mr8[0];
  FSBL_PSRAM_DIAG_RECORD->mr8_r[1] = reg_r_mr8[1];

  if (reg_r_mr8[0] != reg_w_mr8[0])
  {
    FSBL_PSRAM_DIAG_RECORD->expected = reg_w_mr8[0];
    FSBL_PSRAM_DIAG_RECORD->actual = reg_r_mr8[0];
    FSBL_PSRAM_DiagSetFail(FSBL_PSRAM_STEP_READ_MR8, FSBL_PSRAM_ERROR_VERIFY_MR8);
    return HAL_ERROR;
  }

  return HAL_OK;
}

static void FSBL_PSRAM_DiagReset(void)
{
  volatile uint32_t *record = (volatile uint32_t *)FSBL_PSRAM_DIAG_RECORD;
  uint32_t i;

  for (i = 0U; i < (sizeof(FSBL_PSRAM_DiagRecord_t) / sizeof(uint32_t)); i++)
  {
    record[i] = 0U;
  }

  FSBL_PSRAM_DIAG_RECORD->magic = FSBL_PSRAM_DIAG_RECORD_MAGIC;
  FSBL_PSRAM_DIAG_RECORD->version = FSBL_PSRAM_DIAG_RECORD_VERSION;
  FSBL_PSRAM_DIAG_RECORD->diag_level = FSBL_PSRAM_DIAG_LEVEL;
  FSBL_PSRAM_DIAG_RECORD->result = FSBL_PSRAM_DIAG_RESULT_RUNNING;
  FSBL_PSRAM_DIAG_RECORD->step = FSBL_PSRAM_STEP_START;
}

static void FSBL_PSRAM_DiagSetStep(uint32_t step)
{
  FSBL_PSRAM_DIAG_RECORD->step = step;
}

static void FSBL_PSRAM_DiagSetFail(uint32_t step, uint32_t error_code)
{
  FSBL_PSRAM_DIAG_RECORD->result = FSBL_PSRAM_DIAG_RESULT_FAIL;
  FSBL_PSRAM_DIAG_RECORD->step = step;
  FSBL_PSRAM_DIAG_RECORD->error_code = error_code;
  FSBL_PSRAM_DIAG_RECORD->hal_error = HAL_XSPI_GetError(&hxspi1);
}

static void FSBL_PSRAM_DiagSetOk(void)
{
  FSBL_PSRAM_DIAG_RECORD->result = FSBL_PSRAM_DIAG_RESULT_OK;
  FSBL_PSRAM_DIAG_RECORD->step = FSBL_PSRAM_STEP_DONE;
  FSBL_PSRAM_DIAG_RECORD->error_code = FSBL_PSRAM_ERROR_NONE;
  FSBL_PSRAM_DIAG_RECORD->hal_error = HAL_XSPI_GetError(&hxspi1);
}

static void FSBL_PSRAM_RecordXspiBeforeMmap(void)
{
  FSBL_PSRAM_DIAG_RECORD->xspi_cr_before_mmap = hxspi1.Instance->CR;
  FSBL_PSRAM_DIAG_RECORD->xspi_sr_before_mmap = hxspi1.Instance->SR;
  FSBL_PSRAM_DIAG_RECORD->xspi_dcr1_before_mmap = hxspi1.Instance->DCR1;
  FSBL_PSRAM_DIAG_RECORD->xspi_dcr2_before_mmap = hxspi1.Instance->DCR2;
  FSBL_PSRAM_DIAG_RECORD->xspi_dcr3_before_mmap = hxspi1.Instance->DCR3;
  FSBL_PSRAM_DIAG_RECORD->xspi_dcr4_before_mmap = hxspi1.Instance->DCR4;
  FSBL_PSRAM_DIAG_RECORD->xspi_ccr_before_mmap = hxspi1.Instance->CCR;
  FSBL_PSRAM_DIAG_RECORD->xspi_tcr_before_mmap = hxspi1.Instance->TCR;
  FSBL_PSRAM_DIAG_RECORD->xspi_wccr_before_mmap = hxspi1.Instance->WCCR;
  FSBL_PSRAM_DIAG_RECORD->xspi_wtcr_before_mmap = hxspi1.Instance->WTCR;
}

static void FSBL_PSRAM_RecordXspiAfterMmap(void)
{
  FSBL_PSRAM_DIAG_RECORD->xspi_cr_after_mmap = hxspi1.Instance->CR;
  FSBL_PSRAM_DIAG_RECORD->xspi_sr_after_mmap = hxspi1.Instance->SR;
  FSBL_PSRAM_DIAG_RECORD->xspi_dcr1_after_mmap = hxspi1.Instance->DCR1;
  FSBL_PSRAM_DIAG_RECORD->xspi_dcr2_after_mmap = hxspi1.Instance->DCR2;
  FSBL_PSRAM_DIAG_RECORD->xspi_dcr3_after_mmap = hxspi1.Instance->DCR3;
  FSBL_PSRAM_DIAG_RECORD->xspi_dcr4_after_mmap = hxspi1.Instance->DCR4;
  FSBL_PSRAM_DIAG_RECORD->xspi_ccr_after_mmap = hxspi1.Instance->CCR;
  FSBL_PSRAM_DIAG_RECORD->xspi_tcr_after_mmap = hxspi1.Instance->TCR;
  FSBL_PSRAM_DIAG_RECORD->xspi_wccr_after_mmap = hxspi1.Instance->WCCR;
  FSBL_PSRAM_DIAG_RECORD->xspi_wtcr_after_mmap = hxspi1.Instance->WTCR;
}

static void FSBL_PSRAM_ConfigMcePlainRegion(void)
{
  __HAL_RCC_MCE1_CLK_ENABLE();

  if ((MCE1_REGION1->REGCR & MCE_REGCR_BREN) != 0U)
  {
    CLEAR_BIT(MCE1_REGION1->REGCR, MCE_REGCR_BREN);
  }

  MODIFY_REG(MCE1_REGION1->REGCR, MCE_REGCR_ENC, 0U);
  MCE1_REGION1->SADDR = FSBL_PSRAM_MCE_BASE_ADDR;
  MCE1_REGION1->EADDR = FSBL_PSRAM_MCE_END_ADDR;
  SET_BIT(MCE1_REGION1->REGCR, MCE_REGCR_BREN);
}

static void FSBL_PSRAM_RecordAccessPath(void)
{
  uint32_t ahb3enr = RCC->AHB3ENR;
  uint32_t ahb5enr = RCC->AHB5ENR;

  FSBL_PSRAM_DIAG_RECORD->rcc_ahb3enr = ahb3enr;
  FSBL_PSRAM_DIAG_RECORD->rcc_ahb3ensr = RCC->AHB3ENSR;
  FSBL_PSRAM_DIAG_RECORD->rcc_ahb5enr = ahb5enr;
  FSBL_PSRAM_DIAG_RECORD->rcc_ahb5ensr = RCC->AHB5ENSR;

  if ((ahb5enr & RCC_AHB5ENR_XSPIMEN) != 0U)
  {
    FSBL_PSRAM_DIAG_RECORD->xspim_cr = XSPIM->CR;
  }

  FSBL_PSRAM_DIAG_RECORD->mce1_clock_enabled =
      ((ahb5enr & RCC_AHB5ENR_MCE1EN) != 0U) ? 1U : 0U;
  if (FSBL_PSRAM_DIAG_RECORD->mce1_clock_enabled != 0U)
  {
    FSBL_PSRAM_DIAG_RECORD->mce1_cr = MCE1->CR;
    FSBL_PSRAM_DIAG_RECORD->mce1_sr = MCE1->SR;
    FSBL_PSRAM_DIAG_RECORD->mce1_iasr = MCE1->IASR;
    FSBL_PSRAM_DIAG_RECORD->mce1_iaddr = MCE1->IADDR;
    FSBL_PSRAM_DIAG_RECORD->mce1_region1_regcr = MCE1_REGION1->REGCR;
    FSBL_PSRAM_DIAG_RECORD->mce1_region1_saddr = MCE1_REGION1->SADDR;
    FSBL_PSRAM_DIAG_RECORD->mce1_region1_eaddr = MCE1_REGION1->EADDR;
  }

  FSBL_PSRAM_DIAG_RECORD->risaf_clock_enabled =
      ((ahb3enr & RCC_AHB3ENR_RISAFEN) != 0U) ? 1U : 0U;
  if (FSBL_PSRAM_DIAG_RECORD->risaf_clock_enabled != 0U)
  {
    FSBL_PSRAM_DIAG_RECORD->risaf11_cr = RISAF11->CR;
    FSBL_PSRAM_DIAG_RECORD->risaf11_iasr = RISAF11->IASR;
    FSBL_PSRAM_DIAG_RECORD->risaf11_iaesr = RISAF11->IAR[0].IAESR;
    FSBL_PSRAM_DIAG_RECORD->risaf11_iaddr = RISAF11->IAR[0].IADDR;
    FSBL_PSRAM_DIAG_RECORD->risaf11_region1_cfgr = RISAF11->REG[0].CFGR;
    FSBL_PSRAM_DIAG_RECORD->risaf11_region1_startr = RISAF11->REG[0].STARTR;
    FSBL_PSRAM_DIAG_RECORD->risaf11_region1_endr = RISAF11->REG[0].ENDR;
    FSBL_PSRAM_DIAG_RECORD->risaf11_region1_cidcfgr = RISAF11->REG[0].CIDCFGR;
  }

  FSBL_PSRAM_DIAG_RECORD->sau_ctrl = SAU->CTRL;
  FSBL_PSRAM_DIAG_RECORD->sau_type = SAU->TYPE;
  FSBL_PSRAM_DIAG_RECORD->sau_rnr = SAU->RNR;
  FSBL_PSRAM_DIAG_RECORD->sau_rbar = SAU->RBAR;
  FSBL_PSRAM_DIAG_RECORD->sau_rlar = SAU->RLAR;
  FSBL_PSRAM_DIAG_RECORD->mpu_ctrl = MPU->CTRL;
  FSBL_PSRAM_DIAG_RECORD->mpu_type = MPU->TYPE;
}

static void FSBL_PSRAM_DiagLedInit(void)
{
  GPIO_InitTypeDef gpio_init = {0};

  __HAL_RCC_GPIOO_CLK_ENABLE();

  gpio_init.Pin = LED_Pin;
  gpio_init.Mode = GPIO_MODE_OUTPUT_PP;
  gpio_init.Pull = GPIO_NOPULL;
  gpio_init.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_GPIO_Port, &gpio_init);
  HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);
}

#if FSBL_PSRAM_BLOCK_ON_FAIL
static void FSBL_PSRAM_DiagLedBlink(uint32_t count)
{
  uint32_t i;

  while (1)
  {
    for (i = 0U; i < count; i++)
    {
      HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET);
      HAL_Delay(120U);
      HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);
      HAL_Delay(160U);
    }
    HAL_Delay(900U);
  }
}
#endif

static void FSBL_PSRAM_DiagLedPulse(uint32_t count, uint32_t on_ms, uint32_t off_ms)
{
  uint32_t i;

  for (i = 0U; i < count; i++)
  {
    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET);
    HAL_Delay(on_ms);
    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);
    HAL_Delay(off_ms);
  }
}

static void FSBL_PSRAM_DiagLedPulseOk(void)
{
  FSBL_PSRAM_DiagLedPulse(2U, 50U, 50U);
}
