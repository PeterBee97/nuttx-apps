/****************************************************************************
 * apps/testing/titan_asr/asr_bench.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/clock.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>

#include "asr_ctc.h"
#include "asr_mve.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static float scalar_polynomial(float value)
{
  union
  {
    int32_t bits;
    float value;
  } power;

  int32_t exponent;
  float reduced;
  float result;

  if (value < -87.0f)
    {
      return expf(value);
    }

  exponent = (int32_t)(value * 1.4426950408889634f - 0.5f);
  reduced = value - (float)exponent * 0.6931471805599453f;
  result = 1.0f / 720.0f;
  result = __builtin_fmaf(result, reduced, 1.0f / 120.0f);
  result = __builtin_fmaf(result, reduced, 1.0f / 24.0f);
  result = __builtin_fmaf(result, reduced, 1.0f / 6.0f);
  result = __builtin_fmaf(result, reduced, 0.5f);
  result = __builtin_fmaf(result, reduced, 1.0f);
  result = __builtin_fmaf(result, reduced, 1.0f);
  power.bits = (exponent + 127) << 23;
  return result * power.value;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void asr_bench_kernels(const int8_t *logits, float scale, int zero,
                       unsigned int index)
{
  float expanded[ASR_CTC_CLASSES];
  float output[ASR_CTC_CLASSES];
  double precise[ASR_CTC_CLASSES];
  uint64_t elapsed[6] =
  {
    0
  };

  uint32_t start;
  unsigned int frame;
  unsigned int repeat;
  unsigned int mode;
  unsigned int token;
  float maximum;
  double shifted;
  const int8_t *input;

  for (frame = 0; frame < ASR_CTC_FRAMES; frame++)
    {
      input = logits + frame * ASR_CTC_CLASSES;
      asr_mve_expand(input, scale, zero, expanded, ASR_CTC_CLASSES);
      maximum = -INFINITY;
      for (token = 0; token < ASR_CTC_CLASSES; token++)
        {
          if (expanded[token] > maximum)
            {
              maximum = expanded[token];
            }
        }

      for (mode = 0; mode < 6; mode++)
        {
          start = perf_gettime();
          for (repeat = 0; repeat < 16; repeat++)
            {
              if (mode == 0)
                {
                  for (token = 0; token < ASR_CTC_CLASSES; token++)
                    {
                      output[token] = (float)(input[token] - zero) * scale;
                    }
                }
              else if (mode == 1)
                {
                  asr_mve_expand(input, scale, zero, output,
                                 ASR_CTC_CLASSES);
                }
              else if (mode == 2)
                {
                  for (token = 0; token < ASR_CTC_CLASSES; token++)
                    {
                      shifted = (double)expanded[token] - maximum;
                      output[token] = expf((float)shifted);
                    }
                }
              else if (mode == 3)
                {
                  asr_mve_exponentials(expanded, maximum, output,
                                       ASR_CTC_CLASSES);
                }
              else if (mode == 4)
                {
                  for (token = 0; token < ASR_CTC_CLASSES; token++)
                    {
                      precise[token] = exp((double)expanded[token] -
                                           maximum);
                    }
                }
              else
                {
                  for (token = 0; token < ASR_CTC_CLASSES; token++)
                    {
                      output[token] = scalar_polynomial(
                        expanded[token] - maximum);
                    }
                }

              __asm__ __volatile__("" : : "m"(output), "m"(precise) :
                                   "memory");
            }

          elapsed[mode] += (uint32_t)perf_gettime() - start;
        }
    }

  for (mode = 0; mode < 6; mode++)
    {
      printf("ASR_MICRO index=%u mode=%u iterations=16 frames=100 "
             "cycles=%" PRIu64 " us=%" PRIu64 "\n",
             index, mode, elapsed[mode],
             elapsed[mode] * 1000000 / perf_getfreq());
    }
}
