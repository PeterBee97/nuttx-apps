/****************************************************************************
 * apps/audioutils/titan_voicefe/voicefe_dsp.c
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
#include <audioutils/titan_voicefe.h>

/****************************************************************************
 * Public Functions
 ****************************************************************************/

uint32_t voicefe_isqrt(uint64_t value)
{
  uint64_t bit = UINT64_C(1) << 62;
  uint64_t root = 0;

  while (bit > value)
    {
      bit >>= 2;
    }

  while (bit != 0)
    {
      if (value >= root + bit)
        {
          value -= root + bit;
          root = (root >> 1) + bit;
        }
      else
        {
          root >>= 1;
        }

      bit >>= 2;
    }

  return root;
}

void voicefe_dsp_init(struct voicefe_dsp_s *state, int mode,
                     int32_t left_gain, int32_t right_gain)
{
  memset(state, 0, sizeof(*state));
  state->mode = mode;
  state->gain[0] = left_gain;
  state->gain[1] = right_gain;
  state->noise = 400;
}

int voicefe_delay(const int16_t *stereo, size_t frames)
{
  int64_t best = INT64_MIN;
  int64_t cross;
  int lag;
  int result = 0;
  size_t i;

  /* Use the same interior window for every lag. Silence selects zero. */

  for (lag = -VOICEFE_DELAY; lag <= VOICEFE_DELAY; lag++)
    {
      cross = 0;
      for (i = VOICEFE_DELAY; i + VOICEFE_DELAY < frames; i++)
        {
          cross += (int64_t)stereo[2 * i] *
                   stereo[2 * ((int)i + lag) + 1];
        }

      if (cross > best || (cross == best && lag == 0))
        {
          best = cross;
          result = lag;
        }
    }

  return result;
}

void voicefe_process(struct voicefe_dsp_s *state, const int16_t *stereo,
                     int16_t *mono)
{
  const unsigned int length = VOICEFE_DELAY * 2 + 1;
  int32_t input;
  int32_t value;
  int32_t output[2];
  unsigned int delay;
  unsigned int index;
  unsigned int channel;
  unsigned int i;

  for (i = 0; i < VOICEFE_FRAMES; i++)
    {
      for (channel = 0; channel < 2; channel++)
        {
          input = stereo[2 * i + channel];
          if (input >= 32760 || input <= -32760)
            {
              state->clipped[channel]++;
            }

          /* Q15 single-pole DC blocker, pole 0.995. */

          value = input - state->previous[channel] +
                  (int32_t)((int64_t)state->dc[channel] * 32604 / 32768);
          state->previous[channel] = input;
          state->dc[channel] = value;
          value = (int32_t)((int64_t)value * state->gain[channel] / 32768);
          if (value > INT16_MAX)
            {
              value = INT16_MAX;
            }
          else if (value < INT16_MIN)
            {
              value = INT16_MIN;
            }

          state->filtered[i][channel] = value;
        }
    }

  if (state->mode == VOICEFE_BEAM && state->blocks % 10 == 0)
    {
      state->lag = voicefe_delay(&state->filtered[0][0], VOICEFE_FRAMES);
    }

  for (i = 0; i < VOICEFE_FRAMES; i++)
    {
      for (channel = 0; channel < 2; channel++)
        {
          state->history[channel][state->cursor] =
            state->filtered[i][channel];
          delay = VOICEFE_DELAY;
          if (state->mode == VOICEFE_BEAM)
            {
              delay += channel == 0 ? state->lag : 0;
            }

          index = (state->cursor + length - delay) % length;
          output[channel] = state->history[channel][index];
        }

      mono[i] = state->mode < VOICEFE_MEAN ? output[state->mode] :
                (output[0] + output[1]) / 2;
      state->cursor = (state->cursor + 1) % length;
    }

  state->blocks++;
}

int voicefe_speech(struct voicefe_dsp_s *state, const int16_t *mono)
{
  uint64_t energy = 0;
  uint32_t threshold;
  unsigned int i;
  int speech;

  for (i = 0; i < VOICEFE_FRAMES; i++)
    {
      energy += (int64_t)mono[i] * mono[i];
    }

  state->energy = energy / VOICEFE_FRAMES;
  threshold = state->noise > UINT32_MAX / 3 ? UINT32_MAX :
              state->noise * 3;
  if (threshold < 10000)
    {
      threshold = 10000;
    }

  speech = state->energy > threshold;
  if (state->blocks <= 50 || !speech)
    {
      state->noise = ((uint64_t)state->noise * 63 + state->energy) / 64;
    }

  return state->blocks > 50 && speech;
}
