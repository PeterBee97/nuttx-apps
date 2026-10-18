/****************************************************************************
 * apps/testing/titan_asr/asr_mve.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef ASR_MVE_H
#define ASR_MVE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

bool asr_mve_enabled(void);
void asr_mve_enable(bool enabled);
void asr_mve_expand(const int8_t *input, float scale, int zero,
                    float *output, unsigned int count);
void asr_mve_exponentials(const float *input, float maximum, float *output,
                          unsigned int count);

#endif /* ASR_MVE_H */
