/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    dcmipp.c
  * @brief   This file provides code for the configuration
  *          of the DCMIPP instances.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "dcmipp.h"

/* USER CODE BEGIN 0 */
#include "app_console.h"

#define APP_DCMIPP_IRQ_PRIORITY 10U

#define APP_DCMIPP_CSI_DPHY_DATA_LANE_ERROR_IT                               \
  (DCMIPP_CSI_IT_ECTRLDL1 | DCMIPP_CSI_IT_ESYNCESCDL1 | DCMIPP_CSI_IT_EESCDL1 | \
   DCMIPP_CSI_IT_ESOTSYNCDL1 | DCMIPP_CSI_IT_ESOTDL1 | DCMIPP_CSI_IT_ECTRLDL0 | \
   DCMIPP_CSI_IT_ESYNCESCDL0 | DCMIPP_CSI_IT_EESCDL0 | DCMIPP_CSI_IT_ESOTSYNCDL0 | \
   DCMIPP_CSI_IT_ESOTDL0)

#define APP_DCMIPP_CSI_DPHY_DATA_LANE_ERROR_FLAG                               \
  (DCMIPP_CSI_FLAG_ECTRLDL1 | DCMIPP_CSI_FLAG_ESYNCESCDL1 | DCMIPP_CSI_FLAG_EESCDL1 | \
   DCMIPP_CSI_FLAG_ESOTSYNCDL1 | DCMIPP_CSI_FLAG_ESOTDL1 | DCMIPP_CSI_FLAG_ECTRLDL0 | \
   DCMIPP_CSI_FLAG_ESYNCESCDL0 | DCMIPP_CSI_FLAG_EESCDL0 | DCMIPP_CSI_FLAG_ESOTSYNCDL0 | \
   DCMIPP_CSI_FLAG_ESOTDL0)

#define APP_DCMIPP_CSI_COMMON_ERROR_IT \
  (DCMIPP_CSI_IT_CCFIFO | DCMIPP_CSI_IT_SYNCERR | DCMIPP_CSI_IT_SPKTERR | \
   DCMIPP_CSI_IT_IDERR | DCMIPP_CSI_IT_SPKT)

#define APP_DCMIPP_CSI_COMMON_ERROR_FLAG \
  (DCMIPP_CSI_FLAG_CCFIFO | DCMIPP_CSI_FLAG_SYNCERR | DCMIPP_CSI_FLAG_SPKTERR | \
   DCMIPP_CSI_FLAG_IDERR | DCMIPP_CSI_FLAG_SPKT)

static void App_DCMIPP_ClearCsiFlags(void);
static void App_DCMIPP_BusyDelay(uint32_t cycles);
static void App_DCMIPP_CSI_WritePHYReg(uint32_t reg_msb, uint32_t reg_lsb, uint32_t value);
static HAL_StatusTypeDef App_DCMIPP_CSI_SetConfigNoIrq(uint32_t *failed_stage);

/* USER CODE END 0 */

DCMIPP_HandleTypeDef hdcmipp;

/* DCMIPP init function */
void MX_DCMIPP_Init(void)
{

  /* USER CODE BEGIN DCMIPP_Init 0 */

  /* USER CODE END DCMIPP_Init 0 */

  DCMIPP_CSI_PIPE_ConfTypeDef pCSI_PipeConfig = {0};
  DCMIPP_CSI_ConfTypeDef pCSI_Config = {0};
  DCMIPP_PipeConfTypeDef pPipeConfig = {0};

  /* USER CODE BEGIN DCMIPP_Init 1 */

  /* USER CODE END DCMIPP_Init 1 */
  hdcmipp.Instance = DCMIPP;
  if (HAL_DCMIPP_Init(&hdcmipp) != HAL_OK)
  {
    Error_Handler();
  }

  /** Pipe 1 Config
  */
  pCSI_PipeConfig.DataTypeMode = DCMIPP_DTMODE_DTIDA;
  pCSI_PipeConfig.DataTypeIDA = DCMIPP_DT_RAW10;
  pCSI_PipeConfig.DataTypeIDB = DCMIPP_DT_RAW10;
  if (HAL_DCMIPP_CSI_PIPE_SetConfig(&hdcmipp, DCMIPP_PIPE1, &pCSI_PipeConfig) != HAL_OK)
  {
    Error_Handler();
  }
  pCSI_Config.PHYBitrate = DCMIPP_CSI_PHY_BT_450;
  pCSI_Config.DataLaneMapping = DCMIPP_CSI_PHYSICAL_DATA_LANES;
  pCSI_Config.NumberOfLanes = DCMIPP_CSI_TWO_DATA_LANES;
  HAL_DCMIPP_CSI_SetConfig(&hdcmipp, &pCSI_Config);
  __HAL_DCMIPP_CSI_DPHY_DISABLE_IT(CSI, APP_DCMIPP_CSI_DPHY_DATA_LANE_ERROR_IT);
  pPipeConfig.FrameRate = DCMIPP_FRAME_RATE_ALL;
  pPipeConfig.PixelPipePitch = 1280;
  pPipeConfig.PixelPackerFormat = DCMIPP_PIXEL_PACKER_FORMAT_RGB565_1;
  if (HAL_DCMIPP_PIPE_SetConfig(&hdcmipp, DCMIPP_PIPE1, &pPipeConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_DCMIPP_CSI_SetVCConfig(&hdcmipp, 0U, DCMIPP_CSI_DT_BPP10) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN DCMIPP_Init 2 */

  /* USER CODE END DCMIPP_Init 2 */

}

void HAL_DCMIPP_MspInit(DCMIPP_HandleTypeDef* dcmippHandle)
{

  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};
  RIMC_MasterConfig_t RIMC_master = {0};
  if(dcmippHandle->Instance==DCMIPP)
  {
  /* USER CODE BEGIN DCMIPP_MspInit 0 */

  /* USER CODE END DCMIPP_MspInit 0 */

  /** Initializes the peripherals clock
  */
    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_DCMIPP|RCC_PERIPHCLK_CSI;
    PeriphClkInitStruct.DcmippClockSelection = RCC_DCMIPPCLKSOURCE_IC17;
    PeriphClkInitStruct.ICSelection[RCC_IC17].ClockSelection = RCC_ICCLKSOURCE_PLL4;
    PeriphClkInitStruct.ICSelection[RCC_IC17].ClockDivider = 10;
    PeriphClkInitStruct.ICSelection[RCC_IC18].ClockSelection = RCC_ICCLKSOURCE_PLL4;
    PeriphClkInitStruct.ICSelection[RCC_IC18].ClockDivider = 20;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
    {
      Error_Handler();
    }

    /* DCMIPP clock enable */
    __HAL_RCC_DCMIPP_CLK_ENABLE();
    __HAL_RCC_CSI_CLK_ENABLE();
    __HAL_RCC_CSI_FORCE_RESET();
    __HAL_RCC_CSI_RELEASE_RESET();

    /* DCMIPP interrupt Init */
    HAL_NVIC_SetPriority(DCMIPP_IRQn, APP_DCMIPP_IRQ_PRIORITY, 0);
    HAL_NVIC_EnableIRQ(DCMIPP_IRQn);
    HAL_NVIC_SetPriority(CSI_IRQn, APP_DCMIPP_IRQ_PRIORITY, 0);
    HAL_NVIC_EnableIRQ(CSI_IRQn);
  /* USER CODE BEGIN DCMIPP_MspInit 1 */
    __HAL_RCC_DCMIPP_FORCE_RESET();
    __HAL_RCC_DCMIPP_RELEASE_RESET();
    __HAL_RCC_RAMCFG_CLK_ENABLE();

    __HAL_RCC_RIFSC_CLK_ENABLE();
    RIMC_master.MasterCID = RIF_CID_1;
    RIMC_master.SecPriv = RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV;
    HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_DCMIPP, &RIMC_master);
    HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_DCMIPP,
                                          RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
    HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_CSI,
                                          RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);

  /* USER CODE END DCMIPP_MspInit 1 */
  }
}

void HAL_DCMIPP_MspDeInit(DCMIPP_HandleTypeDef* dcmippHandle)
{

  if(dcmippHandle->Instance==DCMIPP)
  {
  /* USER CODE BEGIN DCMIPP_MspDeInit 0 */

  /* USER CODE END DCMIPP_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_CSI_CLK_DISABLE();
    __HAL_RCC_CSI_FORCE_RESET();
    __HAL_RCC_CSI_RELEASE_RESET();

    /* DCMIPP interrupt Deinit */
    HAL_NVIC_DisableIRQ(DCMIPP_IRQn);
    HAL_NVIC_DisableIRQ(CSI_IRQn);
  /* USER CODE BEGIN DCMIPP_MspDeInit 1 */

  /* USER CODE END DCMIPP_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */
static void App_DCMIPP_ClearCsiFlags(void)
{
  CSI->FCR0 = CSI_FCR0_CLB0F | CSI_FCR0_CLB1F | CSI_FCR0_CLB2F | CSI_FCR0_CLB3F |
              CSI_FCR0_CTIM0F | CSI_FCR0_CTIM1F | CSI_FCR0_CTIM2F | CSI_FCR0_CTIM3F |
              CSI_FCR0_CSOF0F | CSI_FCR0_CSOF1F | CSI_FCR0_CSOF2F | CSI_FCR0_CSOF3F |
              CSI_FCR0_CEOF0F | CSI_FCR0_CEOF1F | CSI_FCR0_CEOF2F | CSI_FCR0_CEOF3F |
              CSI_FCR0_CSPKTF | CSI_FCR0_CCCFIFOFF | CSI_FCR0_CCRCERRF |
              CSI_FCR0_CECCERRF | CSI_FCR0_CCECCERRF | CSI_FCR0_CIDERRF |
              CSI_FCR0_CSPKTERRF | CSI_FCR0_CWDERRF | CSI_FCR0_CSYNCERRF;

  CSI->FCR1 = CSI_FCR1_CESOTDL0F | CSI_FCR1_CESOTSYNCDL0F | CSI_FCR1_CEESCDL0F |
              CSI_FCR1_CESYNCESCDL0F | CSI_FCR1_CECTRLDL0F |
              CSI_FCR1_CESOTDL1F | CSI_FCR1_CESOTSYNCDL1F | CSI_FCR1_CEESCDL1F |
              CSI_FCR1_CESYNCESCDL1F | CSI_FCR1_CECTRLDL1F;
}

static void App_DCMIPP_BusyDelay(uint32_t cycles)
{
  for (volatile uint32_t wait = 0U; wait < cycles; wait++)
  {
    __NOP();
  }
}

static void App_DCMIPP_CSI_WritePHYReg(uint32_t reg_msb, uint32_t reg_lsb, uint32_t value)
{
  SET_BIT(CSI->PTCR1, CSI_PTCR1_TWM);
  SET_BIT(CSI->PTCR0, CSI_PTCR0_TCKEN);
  SET_BIT(CSI->PTCR1, CSI_PTCR1_TWM);
  CLEAR_REG(CSI->PTCR0);
  CLEAR_REG(CSI->PTCR1);

  SET_BIT(CSI->PTCR1, reg_msb & 0xFFU);
  SET_BIT(CSI->PTCR0, CSI_PTCR0_TCKEN);
  CLEAR_REG(CSI->PTCR0);

  SET_BIT(CSI->PTCR1, CSI_PTCR1_TWM);
  SET_BIT(CSI->PTCR0, CSI_PTCR0_TCKEN);
  SET_BIT(CSI->PTCR1, CSI_PTCR1_TWM | (reg_lsb & 0xFFU));
  CLEAR_REG(CSI->PTCR0);
  CLEAR_REG(CSI->PTCR1);

  SET_BIT(CSI->PTCR1, value & 0xFFU);
  SET_BIT(CSI->PTCR0, CSI_PTCR0_TCKEN);
  CLEAR_REG(CSI->PTCR0);
}

static HAL_StatusTypeDef App_DCMIPP_CSI_SetConfigNoIrq(uint32_t *failed_stage)
{
  const uint32_t phy_hsfreqrange = 0x16U;
  const uint32_t phy_osc_target = 460U;

  App_Print("DCMIPP diag stage 4.1: csi access\r\n");
  CLEAR_BIT(CSI->CR, CSI_CR_CSIEN);

  App_Print("DCMIPP diag stage 4.2: lane mapping\r\n");
  WRITE_REG(CSI->LMCFGR, DCMIPP_CSI_TWO_DATA_LANES |
                         (DCMIPP_CSI_DATA_LANE0 << CSI_LMCFGR_DL0MAP_Pos) |
                         (DCMIPP_CSI_DATA_LANE1 << CSI_LMCFGR_DL1MAP_Pos));

  App_Print("DCMIPP diag stage 4.3: csi enable\r\n");
  SET_BIT(CSI->CR, CSI_CR_CSIEN);
  __HAL_DCMIPP_CSI_DISABLE_IT(CSI, APP_DCMIPP_CSI_COMMON_ERROR_IT);
  __HAL_DCMIPP_CSI_DPHY_DISABLE_IT(CSI, APP_DCMIPP_CSI_DPHY_DATA_LANE_ERROR_IT);
  App_DCMIPP_ClearCsiFlags();

  App_Print("DCMIPP diag stage 4.4: dphy reset\r\n");
  CLEAR_BIT(CSI->PRCR, CSI_PRCR_PEN);
  CLEAR_REG(CSI->PCR);

  App_Print("DCMIPP diag stage 4.5: testclk\r\n");
  SET_BIT(CSI->PTCR0, CSI_PTCR0_TCKEN);
  App_DCMIPP_BusyDelay(1000U);
  CLEAR_REG(CSI->PTCR0);

  App_Print("DCMIPP diag stage 4.6: pfcr\r\n");
  WRITE_REG(CSI->PFCR, (0x28U << CSI_PFCR_CCFR_Pos) |
                       (phy_hsfreqrange << CSI_PFCR_HSFR_Pos));

  App_Print("DCMIPP diag stage 4.7: phy reg 08\r\n");
  App_DCMIPP_CSI_WritePHYReg(0x00U, 0x08U, 0x38U);

  App_Print("DCMIPP diag stage 4.8: phy reg e4\r\n");
  App_DCMIPP_CSI_WritePHYReg(0x00U, 0xE4U, 0x11U);

  App_Print("DCMIPP diag stage 4.9: phy reg e3\r\n");
  App_DCMIPP_CSI_WritePHYReg(0x00U, 0xE3U, phy_osc_target >> 8);
  App_DCMIPP_CSI_WritePHYReg(0x00U, 0xE3U, phy_osc_target & 0xFFU);

  App_Print("DCMIPP diag stage 4.10: dphy enable\r\n");
  WRITE_REG(CSI->PFCR, (0x28U << CSI_PFCR_CCFR_Pos) |
                       (phy_hsfreqrange << CSI_PFCR_HSFR_Pos) |
                       CSI_PFCR_DLD);
  WRITE_REG(CSI->PCR, CSI_PCR_DL0EN | CSI_PCR_DL1EN | CSI_PCR_CLEN | CSI_PCR_PWRDOWN);
  SET_BIT(CSI->PRCR, CSI_PRCR_PEN);
  CLEAR_REG(CSI->PMCR);

  __HAL_DCMIPP_CSI_DISABLE_IT(CSI, APP_DCMIPP_CSI_COMMON_ERROR_IT);
  __HAL_DCMIPP_CSI_DPHY_DISABLE_IT(CSI, APP_DCMIPP_CSI_DPHY_DATA_LANE_ERROR_IT);
  App_DCMIPP_ClearCsiFlags();
  if (failed_stage != NULL)
  {
    *failed_stage = 0U;
  }

  return HAL_OK;
}

void App_DCMIPP_SetIrqEnabled(uint32_t enabled)
{
  if (enabled != 0U)
  {
    App_DCMIPP_ClearCsiFlags();
    HAL_NVIC_ClearPendingIRQ(DCMIPP_IRQn);
    HAL_NVIC_ClearPendingIRQ(CSI_IRQn);
    HAL_NVIC_SetPriority(DCMIPP_IRQn, APP_DCMIPP_IRQ_PRIORITY, 0);
    HAL_NVIC_EnableIRQ(DCMIPP_IRQn);
    HAL_NVIC_SetPriority(CSI_IRQn, APP_DCMIPP_IRQ_PRIORITY, 0);
    HAL_NVIC_EnableIRQ(CSI_IRQn);
  }
  else
  {
    HAL_NVIC_DisableIRQ(DCMIPP_IRQn);
    HAL_NVIC_DisableIRQ(CSI_IRQn);
    HAL_NVIC_ClearPendingIRQ(DCMIPP_IRQn);
    HAL_NVIC_ClearPendingIRQ(CSI_IRQn);
  }
}

HAL_StatusTypeDef App_DCMIPP_DiagnosticInit(uint32_t *failed_stage, uint32_t *hal_error)
{
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};
  RIMC_MasterConfig_t RIMC_master = {0};
  DCMIPP_CSI_PIPE_ConfTypeDef pCSI_PipeConfig = {0};
  DCMIPP_CSI_ConfTypeDef pCSI_Config = {0};
  DCMIPP_PipeConfTypeDef pPipeConfig = {0};
  HAL_StatusTypeDef status;

  if (failed_stage != NULL)
  {
    *failed_stage = 0U;
  }
  if (hal_error != NULL)
  {
    *hal_error = 0U;
  }

  App_Print("DCMIPP diag stage 1: rif/clock\r\n");
  App_DCMIPP_SetIrqEnabled(0U);

  __HAL_RCC_RIFSC_CLK_ENABLE();
  RIMC_master.MasterCID = RIF_CID_1;
  RIMC_master.SecPriv = RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV;
  HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_DCMIPP, &RIMC_master);
  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_DCMIPP,
                                        RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_CSI,
                                        RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);

  PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_DCMIPP|RCC_PERIPHCLK_CSI;
  PeriphClkInitStruct.DcmippClockSelection = RCC_DCMIPPCLKSOURCE_IC17;
  PeriphClkInitStruct.ICSelection[RCC_IC17].ClockSelection = RCC_ICCLKSOURCE_PLL4;
  PeriphClkInitStruct.ICSelection[RCC_IC17].ClockDivider = 10;
  PeriphClkInitStruct.ICSelection[RCC_IC18].ClockSelection = RCC_ICCLKSOURCE_PLL4;
  PeriphClkInitStruct.ICSelection[RCC_IC18].ClockDivider = 20;
  status = HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct);
  if (status != HAL_OK)
  {
    if (failed_stage != NULL)
    {
      *failed_stage = 1U;
    }
    return status;
  }

  __HAL_RCC_DCMIPP_CLK_ENABLE();
  __HAL_RCC_CSI_CLK_ENABLE();
  __HAL_RCC_DCMIPP_FORCE_RESET();
  __HAL_RCC_DCMIPP_RELEASE_RESET();
  __HAL_RCC_CSI_FORCE_RESET();
  __HAL_RCC_CSI_RELEASE_RESET();
  __HAL_RCC_RAMCFG_CLK_ENABLE();

  App_Print("DCMIPP diag stage 2: hal init\r\n");
  hdcmipp.Instance = DCMIPP;
  hdcmipp.State = HAL_DCMIPP_STATE_BUSY;
  status = HAL_DCMIPP_Init(&hdcmipp);
  if (status != HAL_OK)
  {
    if (failed_stage != NULL)
    {
      *failed_stage = 2U;
    }
    if (hal_error != NULL)
    {
      *hal_error = HAL_DCMIPP_GetError(&hdcmipp);
    }
    return status;
  }

  App_Print("DCMIPP diag stage 3: csi pipe\r\n");
  pCSI_PipeConfig.DataTypeMode = DCMIPP_DTMODE_DTIDA;
  pCSI_PipeConfig.DataTypeIDA = DCMIPP_DT_RAW10;
  pCSI_PipeConfig.DataTypeIDB = DCMIPP_DT_RAW10;
  status = HAL_DCMIPP_CSI_PIPE_SetConfig(&hdcmipp, DCMIPP_PIPE1, &pCSI_PipeConfig);
  if (status != HAL_OK)
  {
    if (failed_stage != NULL)
    {
      *failed_stage = 3U;
    }
    if (hal_error != NULL)
    {
      *hal_error = HAL_DCMIPP_GetError(&hdcmipp);
    }
    return status;
  }

  App_Print("DCMIPP diag stage 4: csi phy\r\n");
  (void)pCSI_Config;
  status = App_DCMIPP_CSI_SetConfigNoIrq(failed_stage);
  if (status != HAL_OK)
  {
    if (failed_stage != NULL)
    {
      if (*failed_stage == 0U)
      {
        *failed_stage = 4U;
      }
    }
    if (hal_error != NULL)
    {
      *hal_error = HAL_DCMIPP_GetError(&hdcmipp);
    }
    return status;
  }
  __HAL_DCMIPP_CSI_DPHY_DISABLE_IT(CSI, APP_DCMIPP_CSI_DPHY_DATA_LANE_ERROR_IT);
  __HAL_DCMIPP_CSI_CLEAR_DPHY_FLAG(CSI, APP_DCMIPP_CSI_DPHY_DATA_LANE_ERROR_FLAG);

  App_Print("DCMIPP diag stage 5: pipe config\r\n");
  pPipeConfig.FrameRate = DCMIPP_FRAME_RATE_ALL;
  pPipeConfig.PixelPipePitch = 1280;
  pPipeConfig.PixelPackerFormat = DCMIPP_PIXEL_PACKER_FORMAT_RGB565_1;
  status = HAL_DCMIPP_PIPE_SetConfig(&hdcmipp, DCMIPP_PIPE1, &pPipeConfig);
  if (status != HAL_OK)
  {
    if (failed_stage != NULL)
    {
      *failed_stage = 5U;
    }
    if (hal_error != NULL)
    {
      *hal_error = HAL_DCMIPP_GetError(&hdcmipp);
    }
    return status;
  }

  App_Print("DCMIPP diag stage 6: vc config\r\n");
  status = HAL_DCMIPP_CSI_SetVCConfig(&hdcmipp, 0U, DCMIPP_CSI_DT_BPP10);
  if (status != HAL_OK)
  {
    if (failed_stage != NULL)
    {
      *failed_stage = 6U;
    }
    if (hal_error != NULL)
    {
      *hal_error = HAL_DCMIPP_GetError(&hdcmipp);
    }
    return status;
  }

  __HAL_DCMIPP_CSI_DISABLE_IT(CSI, APP_DCMIPP_CSI_COMMON_ERROR_IT);
  __HAL_DCMIPP_CSI_CLEAR_FLAG(CSI, APP_DCMIPP_CSI_COMMON_ERROR_FLAG);
  App_DCMIPP_ClearCsiFlags();
  App_DCMIPP_SetIrqEnabled(0U);
  App_Print("DCMIPP diag stage 7: done\r\n");

  return HAL_OK;
}

/* USER CODE END 1 */
