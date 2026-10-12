/****************************************************************************
 * apps/audioutils/titan_intent/intent_gate.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <string.h>
#include <audioutils/titan_intent.h>

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const uint16_t g_exp2[33] =
{
  32768, 32066, 31379, 30706, 30048, 29405, 28774, 28158,
  27554, 26964, 26386, 25821, 25268, 24726, 24196, 23678,
  23170, 22674, 22188, 21713, 21247, 20792, 20347, 19911,
  19484, 19066, 18658, 18258, 17867, 17484, 17109, 16743,
  16384
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void titan_intent_score(const int8_t *logits, unsigned int classes,
                        uint32_t scale_q20,
                        struct titan_intent_score_s *result)
{
  uint32_t sum = 0;
  uint32_t second = 0;
  uint32_t exponent;
  uint32_t fraction;
  uint32_t value;
  uint32_t power;
  unsigned int top = 0;
  unsigned int i;

  memset(result, 0, sizeof(*result));
  if (classes < 2 || classes > TITAN_INTENT_CLASSES || scale_q20 == 0)
    {
      result->label = TITAN_UNKNOWN;
      return;
    }

  for (i = 1; i < classes; i++)
    {
      if (logits[i] > logits[top])
        {
          top = i;
        }
    }

  for (i = 0; i < classes; i++)
    {
      /* exp(-x) = 2^(-x * log2(e)). The scale is fixed during export.
       * A 32-interval table with interpolation avoids libm in inference.
       */

      exponent = ((uint64_t)(logits[top] - logits[i]) * scale_q20 *
                  5909 + 524288) >> 20;
      power = exponent >> 12;
      fraction = exponent & 4095;
      value = g_exp2[fraction >> 7] -
              ((g_exp2[fraction >> 7] -
                g_exp2[(fraction >> 7) + 1]) * (fraction & 127) >> 7);
      value = power >= 16 ? 0 : value >> power;
      sum += value;
      if (i != top && value > second)
        {
          second = value;
        }
    }

  result->label = top;
  result->score = (32768u * 32768u + sum / 2) / sum;
  result->margin = ((32768u - second) * 32768u + sum / 2) / sum;
}

bool titan_intent_score_accept(const struct titan_intent_score_s *score,
                               unsigned int actions, uint16_t threshold,
                               uint16_t margin)
{
  return score->label < actions && score->score >= threshold &&
         score->margin >= margin;
}

void titan_intent_wake(struct titan_intent_s *ctx,
                       const struct titan_intent_score_s *score)
{
  bool accepted;

  if (!ctx->in_turn || ctx->emitted)
    {
      return;
    }

  accepted = titan_intent_score_accept(score, 2, ctx->policy.wake_score,
                                       ctx->policy.wake_margin);
  ctx->wake_history = ((ctx->wake_history << 1) |
                       (accepted && score->label == TITAN_WAKE_INLINE)) & 7;
  ctx->window_history = ((ctx->window_history << 1) |
                         (accepted &&
                          score->label == TITAN_WAKE_WINDOW)) & 7;
  if (ctx->wake_history == 3 || ctx->wake_history == 5 ||
      ctx->wake_history == 6 || ctx->wake_history == 7)
    {
      ctx->inline_seen = true;
      if (!ctx->window_seen)
        {
          ctx->wake = *score;
        }
    }

  if (ctx->window_history == 3 || ctx->window_history == 5 ||
      ctx->window_history == 6 || ctx->window_history == 7)
    {
      ctx->window_seen = true;
      ctx->wake = *score;
    }
}
