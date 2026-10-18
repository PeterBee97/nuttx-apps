/****************************************************************************
 * apps/testing/titan_asr/asr_math.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef ASR_MATH_H
#define ASR_MATH_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <math.h>

/****************************************************************************
 * Inline Functions
 ****************************************************************************/

#ifdef CONFIG_TESTING_TITAN_ASR_FP32_MATH

static inline float asr_log1pf(float value)
{
  float sum = 1.0f + value;

  if (sum == 1.0f)
    {
      return value;
    }

  if (sum <= 0.0f || !isfinite(sum))
    {
      return logf(sum);
    }

  return logf(sum) * value / (sum - 1.0f);
}

#define exp(value)   expf((float)(value))
#define log(value)   logf((float)(value))
#define log1p(value) asr_log1pf((float)(value))

#endif
#endif /* ASR_MATH_H */
