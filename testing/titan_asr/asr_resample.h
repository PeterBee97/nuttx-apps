/****************************************************************************
 * apps/testing/titan_asr/asr_resample.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef ASR_RESAMPLE_H
#define ASR_RESAMPLE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include "asr_resample_fir.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct asr_resample_s
{
  uint32_t phase;
  int16_t history[24];
  unsigned int cursor;
};

/****************************************************************************
 * Inline Functions
 ****************************************************************************/

static inline bool asr_resample(struct asr_resample_s *state,
                                int16_t input, uint32_t input_rate,
                                int16_t *output)
{
  int32_t value;
  int64_t sum;
  unsigned int i;
  unsigned int index;
  unsigned int fraction;
  bool emitted = false;

  if (input_rate == 16000)
    {
      *output = input;
      return true;
    }

  state->history[state->cursor] = input;
  state->cursor = (state->cursor + 1) % 24;
  state->phase += 16000;
  if (state->phase >= input_rate)
    {
      state->phase -= input_rate;
      fraction = state->phase * 64 / 16000;
      sum = 0;
      for (i = 0; i < 24; i++)
        {
          index = (state->cursor + 23 - i) % 24;
          sum += (int32_t)state->history[index] *
                 g_asr_resample_fir[fraction][i];
        }

      value = sum / 32768;
      if (value > INT16_MAX)
        {
          value = INT16_MAX;
        }
      else if (value < INT16_MIN)
        {
          value = INT16_MIN;
        }

      *output = value;
      emitted = true;
    }

  return emitted;
}

#endif /* ASR_RESAMPLE_H */
