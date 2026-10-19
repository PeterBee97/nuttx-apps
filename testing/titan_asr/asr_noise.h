/****************************************************************************
 * apps/testing/titan_asr/asr_noise.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APPS_TESTING_TITAN_ASR_ASR_NOISE_H
#define __APPS_TESTING_TITAN_ASR_ASR_NOISE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Public Type Definitions
 ****************************************************************************/

struct asr_noise_s
{
  unsigned int confirm;
  unsigned int hold;
};

/****************************************************************************
 * Inline Functions
 ****************************************************************************/

/* This measured speech-band gate is experimental. It does not establish
 * acoustic double-talk detection. Keep the echo suppression floor unchanged.
 */

static inline int asr_noise_floor(struct asr_noise_s *state,
                                 const float *power, const float *echo,
                                 int count, float probability, int floor)
{
  float speech = 0;
  int i;

  for (i = 3; i <= 80 && i < count; i++)
    {
      if (power[i] > 4 * echo[i])
        {
          speech += power[i];
        }
    }

  if (probability >= .90f && speech >= 100000)
    {
      if (state->confirm < 8)
        {
          state->confirm++;
        }

      if (state->confirm >= 8)
        {
          state->hold = 10;
        }
    }
  else
    {
      state->confirm = 0;
      if (state->hold > 0)
        {
          state->hold--;
        }
    }

  return state->hold > 0 ? 0 : floor;
}

#endif /* __APPS_TESTING_TITAN_ASR_ASR_NOISE_H */
