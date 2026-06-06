/**
  ******************************************************************************
  * @file    tiny8ch_1dcnn_data_params.h
  * @author  AST Embedded Analytics Research Platform
  * @date    2026-06-06T23:05:06+0800
  * @brief   AI Tool Automatic Code Generator for Embedded NN computing
  ******************************************************************************
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  ******************************************************************************
  */

#ifndef TINY8CH_1DCNN_DATA_PARAMS_H
#define TINY8CH_1DCNN_DATA_PARAMS_H

#include "ai_platform.h"

/*
#define AI_TINY8CH_1DCNN_DATA_WEIGHTS_PARAMS \
  (AI_HANDLE_PTR(&ai_tiny8ch_1dcnn_data_weights_params[1]))
*/

#define AI_TINY8CH_1DCNN_DATA_CONFIG               (NULL)


#define AI_TINY8CH_1DCNN_DATA_ACTIVATIONS_SIZES \
  { 65536, }
#define AI_TINY8CH_1DCNN_DATA_ACTIVATIONS_SIZE     (65536)
#define AI_TINY8CH_1DCNN_DATA_ACTIVATIONS_COUNT    (1)
#define AI_TINY8CH_1DCNN_DATA_ACTIVATION_1_SIZE    (65536)



#define AI_TINY8CH_1DCNN_DATA_WEIGHTS_SIZES \
  { 21296, }
#define AI_TINY8CH_1DCNN_DATA_WEIGHTS_SIZE         (21296)
#define AI_TINY8CH_1DCNN_DATA_WEIGHTS_COUNT        (1)
#define AI_TINY8CH_1DCNN_DATA_WEIGHT_1_SIZE        (21296)



#define AI_TINY8CH_1DCNN_DATA_ACTIVATIONS_TABLE_GET() \
  (&g_tiny8ch_1dcnn_activations_table[1])

extern ai_handle g_tiny8ch_1dcnn_activations_table[1 + 2];



#define AI_TINY8CH_1DCNN_DATA_WEIGHTS_TABLE_GET() \
  (&g_tiny8ch_1dcnn_weights_table[1])

extern ai_handle g_tiny8ch_1dcnn_weights_table[1 + 2];


#endif    /* TINY8CH_1DCNN_DATA_PARAMS_H */
