/****************************************************************************
 * apps/audioutils/titan_voicefe/voicefe_fft.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "voicefe_fft.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define FFT_Q15 32768

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* cos(2 * pi * n / 512), n = 0..128, Q15. */

static const int16_t g_fft_cos[129] =
{
  32767, 32765, 32757, 32745, 32728, 32705, 32678, 32646,
  32609, 32567, 32521, 32469, 32412, 32351, 32285, 32213,
  32137, 32057, 31971, 31880, 31785, 31685, 31580, 31470,
  31356, 31237, 31113, 30985, 30852, 30714, 30571, 30424,
  30273, 30117, 29956, 29791, 29621, 29447, 29268, 29085,
  28898, 28706, 28510, 28310, 28105, 27896, 27683, 27466,
  27245, 27019, 26790, 26556, 26319, 26077, 25832, 25582,
  25329, 25072, 24811, 24547, 24279, 24007, 23731, 23452,
  23170, 22884, 22594, 22301, 22005, 21705, 21403, 21096,
  20787, 20475, 20159, 19841, 19519, 19195, 18868, 18537,
  18204, 17869, 17530, 17189, 16846, 16499, 16151, 15800,
  15446, 15090, 14732, 14372, 14010, 13645, 13279, 12910,
  12539, 12167, 11793, 11417, 11039, 10659, 10278, 9896,
  9512, 9126, 8739, 8351, 7962, 7571, 7179, 6786,
  6393, 5998, 5602, 5205, 4808, 4410, 4011, 3612,
  3212, 2811, 2410, 2009, 1608, 1206, 804, 402, 0
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int32_t voicefe_fft_cos(unsigned int phase)
{
  phase &= VOICEFE_FFT_SIZE - 1;
  if (phase <= 128)
    {
      return g_fft_cos[phase];
    }
  else if (phase <= 256)
    {
      return -g_fft_cos[256 - phase];
    }
  else if (phase <= 384)
    {
      return -g_fft_cos[phase - 256];
    }

  return g_fft_cos[512 - phase];
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void voicefe_fft(int32_t *real, int32_t *imag, bool inverse)
{
  unsigned int i;
  unsigned int j = 0;
  unsigned int bit;
  unsigned int length;
  unsigned int half;
  unsigned int step;
  unsigned int base;
  unsigned int phase;
  int32_t wr;
  int32_t wi;
  int32_t tr;
  int32_t ti;
  int32_t ar;
  int32_t ai;

  for (i = 1; i < VOICEFE_FFT_SIZE; i++)
    {
      for (bit = VOICEFE_FFT_SIZE / 2; (j & bit) != 0; bit >>= 1)
        {
          j ^= bit;
        }

      j ^= bit;
      if (i < j)
        {
          tr = real[i];
          ti = imag[i];
          real[i] = real[j];
          imag[i] = imag[j];
          real[j] = tr;
          imag[j] = ti;
        }
    }

  for (length = 2; length <= VOICEFE_FFT_SIZE; length *= 2)
    {
      half = length / 2;
      step = VOICEFE_FFT_SIZE / length;
      for (j = 0; j < half; j++)
        {
          phase = j * step;
          wr = voicefe_fft_cos(phase);
          wi = voicefe_fft_cos(128 - phase);
          if (!inverse)
            {
              wi = -wi;
            }

          for (base = j; base < VOICEFE_FFT_SIZE; base += length)
            {
              i = base + half;
              tr = ((int64_t)wr * real[i] -
                    (int64_t)wi * imag[i]) / FFT_Q15;
              ti = ((int64_t)wr * imag[i] +
                    (int64_t)wi * real[i]) / FFT_Q15;
              ar = real[base];
              ai = imag[base];
              if (inverse)
                {
                  real[base] = ar + tr;
                  imag[base] = ai + ti;
                  real[i] = ar - tr;
                  imag[i] = ai - ti;
                }
              else
                {
                  real[base] = (ar + tr) / 2;
                  imag[base] = (ai + ti) / 2;
                  real[i] = (ar - tr) / 2;
                  imag[i] = (ai - ti) / 2;
                }
            }
        }
    }
}
