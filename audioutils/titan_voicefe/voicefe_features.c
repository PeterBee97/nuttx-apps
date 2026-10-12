/****************************************************************************
 * apps/audioutils/titan_voicefe/voicefe_features.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <limits.h>
#include <string.h>
#include <audioutils/titan_voicefe.h>
#include "voicefe_fft.h"
#include "voicefe_feature_tables.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t voicefe_log2(uint32_t value)
{
  unsigned int exponent = 0;
  unsigned int index;
  uint32_t leading = value;

  while (leading >= 2)
    {
      exponent++;
      leading >>= 1;
    }

  if (exponent >= 8)
    {
      index = (value >> (exponent - 8)) - 256;
    }
  else
    {
      index = (value << (8 - exponent)) - 256;
    }

  return exponent * 256 + g_feature_log[index];
}

static void voicefe_features_row(struct voicefe_features_s *state,
                                int8_t *row)
{
  uint64_t power;
  uint64_t sum;
  uint32_t mel;
  int32_t value;
  unsigned int i;
  unsigned int band;
  unsigned int bin;
  unsigned int k;

  memset(state->real, 0, sizeof(state->real));
  memset(state->imag, 0, sizeof(state->imag));
  for (i = 0; i < VOICEFE_FEATURE_WIN; i++)
    {
      state->real[i] = (int32_t)state->pcm[i] * g_feature_hann[i] / 128;
    }

  voicefe_fft(state->real, state->imag, false);
  for (i = 0; i <= VOICEFE_FFT_SIZE / 2; i++)
    {
      power = (int64_t)state->real[i] * state->real[i] +
              (int64_t)state->imag[i] * state->imag[i];
      state->power[i] = (power + 32768) / 65536;
    }

  for (band = 0; band < VOICEFE_MELS; band++)
    {
      sum = 0;
      bin = g_feature_start[band];
      for (k = g_feature_offset[band];
           k < g_feature_offset[band + 1]; k++)
        {
          sum += (uint64_t)state->power[bin++] * g_feature_weights[k];
        }

      mel = sum / 32768;
      value = (int32_t)((voicefe_log2(mel + 1) + 16) / 32) - 128;
      row[band] = value > INT8_MAX ? INT8_MAX : value;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void voicefe_features_init(struct voicefe_features_s *state)
{
  memset(state, 0, sizeof(*state));
}

int voicefe_features_push(struct voicefe_features_s *state,
                         const int16_t *mono, int8_t *row)
{
  memcpy(state->pcm + state->count, mono, VOICEFE_FRAMES * sizeof(*mono));
  state->count += VOICEFE_FRAMES;
  if (state->count < VOICEFE_FEATURE_WIN)
    {
      return 0;
    }

  voicefe_features_row(state, row);
  state->count -= VOICEFE_FEATURE_HOP;
  memmove(state->pcm, state->pcm + VOICEFE_FEATURE_HOP,
          state->count * sizeof(*mono));
  state->frames++;
  return 1;
}

size_t voicefe_features_size(void)
{
  return sizeof(struct voicefe_features_s);
}
