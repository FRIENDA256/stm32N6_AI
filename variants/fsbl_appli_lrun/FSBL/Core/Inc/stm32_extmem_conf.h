/**
  ******************************************************************************
  * @file    stm32_extmem_conf.h
  * @brief   External memory manager configuration for FSBL -> Appli LRUN.
  ******************************************************************************
  */

#ifndef EXTMEM_CONF_H_
#define EXTMEM_CONF_H_

#ifdef __cplusplus
extern "C" {
#endif

#define EXTMEM_DRIVER_NOR_SFDP   1
#define EXTMEM_DRIVER_PSRAM      0
#define EXTMEM_SAL_XSPI          1

#include "stm32n6xx_hal.h"
#include "stm32_extmem.h"
#include "stm32_extmem_type.h"
#include "boot/stm32_boot_lrun.h"

extern XSPI_HandleTypeDef hxspi2;

enum
{
  EXTMEMORY_1 = 0
};

#define EXTMEM_LRUN_DESTINATION_INTERNAL
#define EXTMEM_LRUN_DESTINATION_ADDRESS  0x34000000U

#define EXTMEM_LRUN_SOURCE               EXTMEMORY_1
#define EXTMEM_LRUN_SOURCE_ADDRESS       0x00100000U
#define EXTMEM_LRUN_SOURCE_SIZE          0x00100000U

#define EXTMEM_HEADER_OFFSET             0x400U

extern EXTMEM_DefinitionTypeDef extmem_list_config[1];

#if defined(EXTMEM_C)
EXTMEM_DefinitionTypeDef extmem_list_config[1] =
{
  {
    .MemType = EXTMEM_NOR_SFDP,
    .Handle = (void *)&hxspi2,
    .ConfigType = EXTMEM_LINK_CONFIG_8LINES,
    .NorSfdpObject =
    {
      {0}
    }
  }
};
#endif /* EXTMEM_C */

#define EXTMEM_DEBUG_LEVEL                  0
#define EXTMEM_DRIVER_NOR_SFDP_DEBUG_LEVEL  0
#define EXTMEM_DRIVER_PSRAM_DEBUG_LEVEL     0
#define EXTMEM_SAL_XSPI_DEBUG_LEVEL         0

#ifdef __cplusplus
}
#endif

#endif /* EXTMEM_CONF_H_ */
