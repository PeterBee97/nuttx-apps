/****************************************************************************
 * apps/audioutils/titan_voicefe/voicefe_ns.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <string.h>
#include <limits.h>
#include <stdbool.h>
#include <audioutils/titan_voicefe.h>
#include "voicefe_fft.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NS_FFT   512
#define NS_BINS  (NS_FFT / 2 + 1)
#define NS_Q15   32768

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* First half of sin(pi * (n + 0.5) / 320), Q15. Squared overlap windows
 * sum to unity. The second half is the reversed first half.
 */

static const int16_t g_ns_window[VOICEFE_FRAMES] =
{
  161, 483, 804, 1126, 1447, 1768, 2090, 2410,
  2731, 3052, 3372, 3692, 4011, 4330, 4649, 4967,
  5285, 5602, 5919, 6235, 6550, 6865, 7179, 7493,
  7806, 8118, 8429, 8739, 9049, 9358, 9666, 9972,
  10278, 10583, 10887, 11190, 11492, 11793, 12092, 12391,
  12688, 12984, 13279, 13572, 13864, 14155, 14444, 14732,
  15019, 15304, 15588, 15870, 16151, 16430, 16707, 16983,
  17258, 17530, 17801, 18070, 18338, 18604, 18868, 19130,
  19390, 19648, 19905, 20159, 20412, 20663, 20911, 21158,
  21403, 21645, 21886, 22124, 22360, 22594, 22826, 23056,
  23283, 23508, 23731, 23952, 24170, 24386, 24600, 24811,
  25020, 25227, 25431, 25633, 25832, 26028, 26223, 26414,
  26603, 26790, 26974, 27155, 27334, 27510, 27683, 27854,
  28022, 28188, 28350, 28510, 28667, 28822, 28973, 29122,
  29268, 29412, 29552, 29689, 29824, 29956, 30085, 30211,
  30334, 30454, 30571, 30686, 30797, 30905, 31011, 31113,
  31213, 31309, 31402, 31493, 31580, 31664, 31746, 31824,
  31899, 31971, 32040, 32106, 32168, 32228, 32285, 32338,
  32388, 32436, 32480, 32521, 32558, 32593, 32625, 32653,
  32678, 32700, 32719, 32735, 32748, 32757, 32763, 32767
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void voicefe_ns_init(struct voicefe_ns_s *state, uint16_t floor)
{
  unsigned int i;

  memset(state, 0, sizeof(*state));
  state->floor = floor > NS_Q15 ? NS_Q15 : floor;
  state->gain_q15 = NS_Q15;
  for (i = 0; i < NS_BINS; i++)
    {
      state->gain[i] = NS_Q15;
      state->minimum[i] = UINT32_MAX;
    }
}

void voicefe_ns_process(struct voicefe_ns_s *state, int16_t *mono)
{
  uint64_t noise;
  uint64_t power;
  uint32_t gain;
  uint32_t previous;
  uint32_t right;
  uint32_t sum = 0;
  int32_t value;
  unsigned int i;

  memset(state->real, 0, sizeof(state->real));
  memset(state->imag, 0, sizeof(state->imag));
  for (i = 0; i < VOICEFE_FRAMES; i++)
    {
      /* Q8 extra precision avoids FFT truncation of weak microphone input. */

      state->real[i] = (int32_t)state->previous[i] *
                      g_ns_window[i] / 128;
      state->real[i + VOICEFE_FRAMES] = (int32_t)mono[i] *
                        g_ns_window[VOICEFE_FRAMES - 1 - i] / 128;
      state->previous[i] = mono[i];
    }

  voicefe_fft(state->real, state->imag, false);
  for (i = 0; i < NS_BINS; i++)
    {
      power = (int64_t)state->real[i] * state->real[i] +
              (int64_t)state->imag[i] * state->imag[i];
      power = (power + 32768) / 65536;
      if (state->blocks == 0)
        {
          state->power[i] = power;
        }
      else
        {
          state->power[i] = ((uint64_t)state->power[i] * 3 + power) / 4;
        }

      power = state->power[i];
      if (power < state->minimum[i])
        {
          state->minimum[i] = power;
        }

      noise = state->noise[i];
      if (state->blocks < 50)
        {
          noise = state->blocks == 0 ? power :
                  (noise * 15 + power) / 16;
        }
      else if (power < noise)
        {
          noise = (noise * 7 + power) / 8;
        }
      else if (power < noise * 2 + 4)
        {
          noise = (noise * 127 + power) / 128;
        }

      /* Once per second, use local minima to follow changing background
       * without treating short speech peaks as persistent noise.
       */

      if (state->blocks >= 50 && state->blocks % 100 == 99)
        {
          noise = (noise * 3 + state->minimum[i]) / 4;
          state->minimum[i] = UINT32_MAX;
        }

      state->noise[i] = noise;
      noise = noise * 5 / 4;
      gain = power > noise ? (power - noise) * NS_Q15 / (power + 1) : 0;
      if (gain < state->floor)
        {
          gain = state->floor;
        }

      /* Fast recovery for speech onsets, slower noise attenuation. */

      state->gain[i] = gain > state->gain[i] ?
                       (state->gain[i] + gain * 3) / 4 :
                       (state->gain[i] * 3 + gain) / 4;
    }

  previous = state->gain[0];
  for (i = 0; i < NS_BINS; i++)
    {
      right = state->gain[i + (i + 1 < NS_BINS)];
      gain = (previous + state->gain[i] * 2 + right) / 4;
      previous = state->gain[i];
      sum += gain;
      state->real[i] = (int64_t)state->real[i] * gain / NS_Q15;
      state->imag[i] = (int64_t)state->imag[i] * gain / NS_Q15;
      if (i > 0 && i < NS_FFT / 2)
        {
          state->real[NS_FFT - i] = state->real[i];
          state->imag[NS_FFT - i] = -state->imag[i];
        }
    }

  state->gain_q15 = sum / NS_BINS;
  voicefe_fft(state->real, state->imag, true);
  for (i = 0; i < VOICEFE_FRAMES; i++)
    {
      value = ((int64_t)state->real[i] * g_ns_window[i] / NS_Q15 +
               state->overlap[i]) / 256;
      state->overlap[i] = (int64_t)state->real[i + VOICEFE_FRAMES] *
                         g_ns_window[VOICEFE_FRAMES - 1 - i] / NS_Q15;
      if (value > INT16_MAX || value < INT16_MIN)
        {
          state->clipped++;
          value = value > INT16_MAX ? INT16_MAX : INT16_MIN;
        }

      mono[i] = value;
    }

  state->blocks++;
}
