/****************************************************************************
 * apps/testing/titan_asr/asr_opt.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef ASR_OPT_H
#define ASR_OPT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ASR_OPT_HASH 2
#define ASR_OPT_LM   4
#define ASR_OPT_TOPK 8
#define ASR_HASH_SLOTS 256
#define ASR_PROFILE_LOGADD 0
#define ASR_PROFILE_LM 1
#define ASR_PROFILE_MERGE 2
#define ASR_PROFILE_SETUP 3
#define ASR_PROFILE_RANK 4
#define ASR_PROFILE_COUNT 5

/****************************************************************************
 * Public Data
 ****************************************************************************/

extern unsigned int g_asr_opt_mask;
extern bool g_asr_profile_enabled;
extern uint64_t g_asr_profile_cycles[ASR_PROFILE_COUNT];
extern uint32_t g_asr_profile_calls[ASR_PROFILE_COUNT];

#endif /* ASR_OPT_H */
