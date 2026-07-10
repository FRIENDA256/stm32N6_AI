/**
  ******************************************************************************
  * @file    tiny_temporal_mixer_8ch_int8.h
  * @author  STEdgeAI
  * @date    2026-07-09 22:46:29
  * @brief   Minimal description of the generated c-implemention of the network
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  ******************************************************************************
  */
#ifndef LL_ATON_TINY_TEMPORAL_MIXER_8CH_INT8_H
#define LL_ATON_TINY_TEMPORAL_MIXER_8CH_INT8_H

/******************************************************************************/
#define LL_ATON_TINY_TEMPORAL_MIXER_8CH_INT8_C_MODEL_NAME        "tiny_temporal_mixer_8ch_int8"
#define LL_ATON_TINY_TEMPORAL_MIXER_8CH_INT8_ORIGIN_MODEL_NAME   "tiny_temporal_mixer_8ch_int8_qdq"

/************************** USER ALLOCATED IOs ********************************/
// No user allocated inputs
// No user allocated outputs

/************************** INPUTS ********************************************/
#define LL_ATON_TINY_TEMPORAL_MIXER_8CH_INT8_IN_NUM        (1)    // Total number of input buffers
// Input buffer 1 -- Input_0_out_0
#define LL_ATON_TINY_TEMPORAL_MIXER_8CH_INT8_IN_1_ALIGNMENT   (32)
#define LL_ATON_TINY_TEMPORAL_MIXER_8CH_INT8_IN_1_SIZE_BYTES  (8192)

/************************** OUTPUTS *******************************************/
#define LL_ATON_TINY_TEMPORAL_MIXER_8CH_INT8_OUT_NUM        (1)    // Total number of output buffers
// Output buffer 1 -- Quantize_52_out_0
#define LL_ATON_TINY_TEMPORAL_MIXER_8CH_INT8_OUT_1_ALIGNMENT   (32)
#define LL_ATON_TINY_TEMPORAL_MIXER_8CH_INT8_OUT_1_SIZE_BYTES  (4)

#endif /* LL_ATON_TINY_TEMPORAL_MIXER_8CH_INT8_H */
