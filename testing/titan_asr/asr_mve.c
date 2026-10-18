/****************************************************************************
 * apps/testing/titan_asr/asr_mve.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <arm_mve.h>
#include <math.h>

#include "asr_mve.h"

#if !defined(__ARM_FEATURE_MVE) || (__ARM_FEATURE_MVE & 2) == 0
#  error "ASR MVE requires floating point Helium instructions"
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bool g_enabled = true;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

bool asr_mve_enabled(void)
{
  return g_enabled;
}

void asr_mve_enable(bool enabled)
{
  g_enabled = enabled;
}

void asr_mve_expand(const int8_t *input, float scale, int zero,
                    float *output, unsigned int count)
{
  unsigned int i;
  int32x4_t value;

  for (i = 0; i + 4 <= count; i += 4)
    {
      value = vsubq_n_s32(vldrbq_s32(input + i), zero);
      vstrwq_f32(output + i, vmulq_n_f32(vcvtq_f32_s32(value), scale));
    }

  for (; i < count; i++)
    {
      output[i] = (float)(input[i] - zero) * scale;
    }
}

void asr_mve_exponentials(const float *input, float maximum, float *output,
                          unsigned int count)
{
  float32x4_t value;
  float32x4_t reduced;
  float32x4_t polynomial;
  float32x4_t power;
  int32x4_t exponent;
  mve_pred16_t underflow;
  unsigned int i;

  /* Range reduction to [-ln(2)/2, ln(2)/2], degree six Taylor polynomial.
   * Inputs below -87 use scalar expf to preserve subnormal handling.
   */

  for (i = 0; i + 4 <= count; i += 4)
    {
      value = vsubq_n_f32(vldrwq_f32(input + i), maximum);
      underflow = vcmpltq_n_f32(value, -87.0f);
      value = vmaxnmq_f32(value, vdupq_n_f32(-87.0f));
      exponent = vcvtq_s32_f32(vsubq_n_f32(
        vmulq_n_f32(value, 1.4426950408889634f), 0.5f));
      reduced = vsubq_f32(value, vmulq_n_f32(
        vcvtq_f32_s32(exponent), 0.6931471805599453f));
      polynomial = vdupq_n_f32(1.0f / 720.0f);
      polynomial = vfmaq_f32(vdupq_n_f32(1.0f / 120.0f),
                             polynomial, reduced);
      polynomial = vfmaq_f32(vdupq_n_f32(1.0f / 24.0f),
                             polynomial, reduced);
      polynomial = vfmaq_f32(vdupq_n_f32(1.0f / 6.0f),
                             polynomial, reduced);
      polynomial = vfmaq_f32(vdupq_n_f32(0.5f), polynomial, reduced);
      polynomial = vfmaq_f32(vdupq_n_f32(1.0f), polynomial, reduced);
      polynomial = vfmaq_f32(vdupq_n_f32(1.0f), polynomial, reduced);
      exponent = vmaxq_s32(exponent, vdupq_n_s32(-126));
      power = vreinterpretq_f32_s32(vshlq_n_s32(
        vaddq_n_s32(exponent, 127), 23));
      vstrwq_f32(output + i, vmulq_f32(polynomial, power));
      if (underflow != 0)
        {
          for (unsigned int lane = 0; lane < 4; lane++)
            {
              if (input[i + lane] - maximum < -87.0f)
                {
                  output[i + lane] = expf(input[i + lane] - maximum);
                }
            }
        }
    }

  for (; i < count; i++)
    {
      output[i] = expf(input[i] - maximum);
    }
}
