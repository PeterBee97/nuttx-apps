/****************************************************************************
 * asr_runtime.c
 *
 * Bounded CTC Viterbi confidence and independent full-utterance text grammar.
 * No acoustic weights or intent labels are used by the text classifier.
 ****************************************************************************/

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#include <nuttx/config.h>

#include "asr_runtime.h"
#include "asr_text_tables.h"
#ifdef CONFIG_TESTING_TITAN_ASR_MVE
#  include "asr_mve.h"
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static bool punctuation(uint32_t character)
{
  static const uint32_t marks[] =
  {
    0xff0c, ',', 0x3002, '.', '!', 0xff01, '?', 0xff1f,
    0x3001, 0xff1a, ':', 0xff1b, ';'
  };

  size_t i;

  for (i = 0; i < ARRAY_SIZE(marks); i++)
    {
      if (marks[i] == character)
        {
          return true;
        }
    }

  for (i = 0; i < ARRAY_SIZE(g_asr_whitespace); i++)
    {
      if (g_asr_whitespace[i] == character)
        {
          return true;
        }
    }

  return false;
}

static int utf8_read(const unsigned char **cursor, uint32_t *character)
{
  const unsigned char *source = *cursor;
  uint32_t value;
  uint32_t minimum;
  unsigned int more;
  unsigned int i;

  if (*source < 0x80)
    {
      *character = *source;
      *cursor = source + 1;
      return 0;
    }

  if ((*source & 0xe0) == 0xc0)
    {
      value = *source & 0x1f;
      minimum = 0x80;
      more = 1;
    }
  else if ((*source & 0xf0) == 0xe0)
    {
      value = *source & 0x0f;
      minimum = 0x800;
      more = 2;
    }
  else if ((*source & 0xf8) == 0xf0)
    {
      value = *source & 0x07;
      minimum = 0x10000;
      more = 3;
    }
  else
    {
      return -EINVAL;
    }

  for (i = 1; i <= more; i++)
    {
      if ((source[i] & 0xc0) != 0x80)
        {
          return -EINVAL;
        }

      value = (value << 6) | (source[i] & 0x3f);
    }

  if (value < minimum || value > 0x10ffff ||
      (value >= 0xd800 && value <= 0xdfff))
    {
      return -EINVAL;
    }

  *character = value;
  *cursor = source + more + 1;
  return 0;
}

static unsigned int utf8_write(char *target, uint32_t character)
{
  if (character < 0x80)
    {
      target[0] = character;
      return 1;
    }

  if (character < 0x800)
    {
      target[0] = 0xc0 | (character >> 6);
      target[1] = 0x80 | (character & 0x3f);
      return 2;
    }

  if (character < 0x10000)
    {
      target[0] = 0xe0 | (character >> 12);
      target[1] = 0x80 | ((character >> 6) & 0x3f);
      target[2] = 0x80 | (character & 0x3f);
      return 3;
    }

  target[0] = 0xf0 | (character >> 18);
  target[1] = 0x80 | ((character >> 12) & 0x3f);
  target[2] = 0x80 | ((character >> 6) & 0x3f);
  target[3] = 0x80 | (character & 0x3f);
  return 4;
}

int asr_text_classify(const char *text)
{
  static const uint32_t prefixes[][2] =
  {
    {0x9ebb, 0x70e6}, /* 麻烦 */
    {0x5e2e, 0x5fd9}, /* 帮忙 */
    {0x5e2e, 0x6211}, /* 帮我 */
    {0x7ed9, 0x6211}, /* 给我 */
    {0x73b0, 0x5728}  /* 现在 */
  };

  const unsigned char *source = (const unsigned char *)text;
  const struct asr_dfa_state_s *dfa;
  uint32_t characters[ASR_CTC_FRAMES];
  uint32_t character;
  unsigned int count = 0;
  unsigned int begin = 0;
  unsigned int state = 0;
  unsigned int mask;
  unsigned int label;
  size_t i;
  size_t left;
  size_t right;
  size_t middle;
  bool found;

  if (text == NULL)
    {
      return ASR_UNKNOWN;
    }

  while (*source != 0)
    {
      if (utf8_read(&source, &character) < 0)
        {
          return ASR_UNKNOWN;
        }

      if (!punctuation(character))
        {
          if (count == ASR_CTC_FRAMES)
            {
              return ASR_UNKNOWN;
            }

          characters[count++] = character;
        }
    }

  while (begin + 1 < count &&
         characters[begin] == 0x8c46 && characters[begin + 1] == 0x5305)
    {
      begin += 2;
    }

  while (begin < count)
    {
      if (characters[begin] == 0x8bf7) /* 请 */
        {
          begin++;
          continue;
        }

      found = false;
      for (i = 0; begin + 1 < count && i < ARRAY_SIZE(prefixes); i++)
        {
          if (characters[begin] == prefixes[i][0] &&
              characters[begin + 1] == prefixes[i][1])
            {
              begin += 2;
              found = true;
              break;
            }
        }

      if (!found)
        {
          break;
        }
    }

  if (begin == count)
    {
      return ASR_SILENCE;
    }

  for (i = begin; i < count; i++)
    {
      dfa = &g_asr_text_states[state];
      left = dfa->offset;
      right = left + dfa->count;
      while (left < right)
        {
          middle = left + (right - left) / 2;
          if (g_asr_text_edges[middle].character < characters[i])
            {
              left = middle + 1;
            }
          else
            {
              right = middle;
            }
        }

      if (left == dfa->offset + dfa->count ||
          g_asr_text_edges[left].character != characters[i])
        {
          return ASR_UNKNOWN;
        }

      state = g_asr_text_edges[left].destination;
    }

  mask = g_asr_text_states[state].mask;
  if (mask == 0 || (mask & (mask - 1)) != 0)
    {
      return ASR_UNKNOWN;
    }

  for (label = 0; (mask >> label) != 1; label++)
    {
    }

  return ASR_PHOTO_TAKE + label;
}

static double logit_value(const void *logits, bool quantized,
                          unsigned int frame, unsigned int token,
                          float scale, int zero_point)
{
  unsigned int index = frame * ASR_CTC_CLASSES + token;

  if (quantized)
    {
      return (float)(((const int8_t *)logits)[index] - zero_point) * scale;
    }

  return ((const float *)logits)[index];
}

static unsigned int expanded_token(const struct asr_result_s *result,
                                   unsigned int state)
{
  return (state & 1) != 0 ? result->tokens[state / 2] : 0;
}

static int postprocess(struct asr_alignment_s *workspace,
                       const void *logits, unsigned int frames,
                       bool quantized, float scale, int zero_point,
                       struct asr_result_s *result, double threshold)
{
  double *previous;
  double *updated;
  double maximum;
  double sum;
  double value;
  double best;
  double probability;
  double log_confidence = 0;
  float token_probability;
  unsigned int states;
  unsigned int state;
  unsigned int frame;
  unsigned int token;
  unsigned int origin;
  unsigned int character;
  unsigned int length;
  unsigned int required;
  unsigned int offset = 0;
  unsigned int shift;
  unsigned int cell;
#ifdef CONFIG_TESTING_TITAN_ASR_MVE
  float expanded[ASR_CTC_CLASSES];
  float vector_exp[ASR_CTC_CLASSES];
#endif
#ifdef CONFIG_TESTING_TITAN_ASR_INT8_EXP_CACHE
  double exponentials[256];
  bool cached[256];
  unsigned int index;
#endif

  if (workspace == NULL || logits == NULL || result == NULL ||
      frames == 0 || frames > ASR_CTC_FRAMES ||
      result->length > ASR_CTC_FRAMES ||
      !isfinite(threshold) || threshold < 0 || threshold > 1 ||
      (quantized && (!isfinite(scale) || scale <= 0 ||
                     zero_point < -128 || zero_point > 127)))
    {
      return -EINVAL;
    }

  result->intent = ASR_UNKNOWN;
  result->confidence = 0;
  result->minimum_confidence = 0;
  result->unknown_characters = 0;
  result->text[0] = '\0';
  length = result->length;
  required = length;
  for (token = 0; token < length; token++)
    {
      if (result->tokens[token] == 0 ||
          result->tokens[token] >= ASR_CTC_CLASSES)
        {
          return -EINVAL;
        }

      if (token != 0 && result->tokens[token] == result->tokens[token - 1])
        {
          required++;
        }

      character = g_asr_characters[result->tokens[token]];
      offset += utf8_write(result->text + offset, character);
      result->unknown_characters += result->tokens[token] == 1;
      result->token_confidence[token] = -1;
    }

  result->text[offset] = '\0';
  if (required > frames)
    {
      return -EINVAL;
    }

  if (length == 0)
    {
      result->intent = threshold > 0 ? ASR_UNKNOWN : ASR_SILENCE;
      return 0;
    }

  states = 2 * length + 1;
  memset(workspace->back, 0, sizeof(workspace->back));
  for (state = 0; state < states; state++)
    {
      workspace->scores[0][state] = -INFINITY;
    }

  for (frame = 0; frame < frames; frame++)
    {
#ifdef CONFIG_TESTING_TITAN_ASR_MVE
      if (quantized && asr_mve_enabled())
        {
          asr_mve_expand((const int8_t *)logits + frame * ASR_CTC_CLASSES,
                         scale, zero_point, expanded, ASR_CTC_CLASSES);
        }
#endif
      maximum = -INFINITY;
      for (token = 0; token < ASR_CTC_CLASSES; token++)
        {
          value = logit_value(logits, quantized, frame, token,
                              scale, zero_point);
          if (!isfinite(value))
            {
              return -EINVAL;
            }

          if (value > maximum)
            {
              maximum = value;
            }
        }

      sum = 0;
#ifdef CONFIG_TESTING_TITAN_ASR_MVE
      if (quantized && asr_mve_enabled())
        {
          asr_mve_exponentials(expanded, maximum, vector_exp,
                              ASR_CTC_CLASSES);
        }
#endif
#ifdef CONFIG_TESTING_TITAN_ASR_INT8_EXP_CACHE
      if (quantized)
        {
          memset(cached, 0, sizeof(cached));
        }
#endif

      for (token = 0; token < ASR_CTC_CLASSES; token++)
        {
#ifdef CONFIG_TESTING_TITAN_ASR_MVE
          if (quantized && asr_mve_enabled())
            {
              sum += vector_exp[token];
              continue;
            }
#endif
          value = logit_value(logits, quantized, frame, token,
                              scale, zero_point);
#ifdef CONFIG_TESTING_TITAN_ASR_INT8_EXP_CACHE
          if (quantized)
            {
              index = (uint8_t)((const int8_t *)logits)
                [frame * ASR_CTC_CLASSES + token];
              if (!cached[index])
                {
                  exponentials[index] = exp(value - maximum);
                  cached[index] = true;
                }

              sum += exponentials[index];
              continue;
            }
#endif

          sum += exp(value - maximum);
        }

      workspace->normalizers[frame] = maximum + log(sum);
      updated = workspace->scores[frame & 1];
      if (frame == 0)
        {
          updated[0] = logit_value(logits, quantized, 0, 0,
                                  scale, zero_point) -
                       workspace->normalizers[0];
          updated[1] = logit_value(logits, quantized, 0, result->tokens[0],
                                  scale, zero_point) -
                       workspace->normalizers[0];
          continue;
        }

      previous = workspace->scores[(frame - 1) & 1];
      for (state = 0; state < states; state++)
        {
          token = expanded_token(result, state);
          best = previous[state];
          origin = state;
          if (state != 0 && previous[state - 1] > best)
            {
              best = previous[state - 1];
              origin = state - 1;
            }

          if (state > 1 && token != 0 &&
              token != expanded_token(result, state - 2) &&
              previous[state - 2] > best)
            {
              best = previous[state - 2];
              origin = state - 2;
            }

          updated[state] = best +
            logit_value(logits, quantized, frame, token, scale, zero_point) -
            workspace->normalizers[frame];
          shift = (state & 3) * 2;
          workspace->back[frame][state / 4] |= (state - origin) << shift;
        }
    }

  previous = workspace->scores[(frames - 1) & 1];
  state = states - 1;
  if (previous[states - 2] > previous[state])
    {
      state--;
    }

  if (!isfinite(previous[state]))
    {
      return -EINVAL;
    }

  for (frame = frames; frame-- > 0; )
    {
      if ((state & 1) != 0)
        {
          token = state / 2;
          probability = exp(
            logit_value(logits, quantized, frame, result->tokens[token],
                        scale, zero_point) - workspace->normalizers[frame]);
          token_probability = probability;
          /* Backtracking is reverse: prefer the earliest equally high peak. */

          if (token_probability >= result->token_confidence[token])
            {
              result->token_confidence[token] = token_probability;
              result->frames[token] = frame;
            }
        }

      cell = workspace->back[frame][state / 4];
      state -= (cell >> ((state & 3) * 2)) & 3;
    }

  result->minimum_confidence = 1;
  for (token = 0; token < length; token++)
    {
      probability = result->token_confidence[token];
      if (probability < 0)
        {
          return -EINVAL;
        }

      if (probability < result->minimum_confidence)
        {
          result->minimum_confidence = probability;
        }

      log_confidence += log(probability > 1e-8 ? probability : 1e-8);
    }

  result->confidence = exp(log_confidence / length);
  if (result->unknown_characters == 0 && result->confidence >= threshold)
    {
      result->intent = asr_text_classify(result->text);
    }

  return 0;
}

int asr_postprocess_float(struct asr_alignment_s *workspace,
                          const float *logits, unsigned int frames,
                          struct asr_result_s *result, double threshold)
{
  return postprocess(workspace, logits, frames, false, 1, 0,
                     result, threshold);
}

int asr_postprocess_int8(struct asr_alignment_s *workspace,
                         const int8_t *logits, unsigned int frames,
                         float scale, int zero_point,
                         struct asr_result_s *result, double threshold)
{
  return postprocess(workspace, logits, frames, true, scale, zero_point,
                     result, threshold);
}

int asr_ctc_frame_int8(struct asr_ctc_s *state, const int8_t *logits,
                      float scale, int zero_point)
{
  float expanded[ASR_CTC_CLASSES];
  unsigned int token;

  if (logits == NULL || !isfinite(scale) || scale <= 0 ||
      zero_point < -128 || zero_point > 127)
    {
      return -EINVAL;
    }

#ifdef CONFIG_TESTING_TITAN_ASR_MVE
  if (asr_mve_enabled())
    {
      asr_mve_expand(logits, scale, zero_point, expanded, ASR_CTC_CLASSES);
      return asr_ctc_frame(state, expanded);
    }
#endif

  for (token = 0; token < ASR_CTC_CLASSES; token++)
    {
      expanded[token] = (float)(logits[token] - zero_point) * scale;
    }

  return asr_ctc_frame(state, expanded);
}

size_t asr_alignment_bytes(void)
{
  return sizeof(struct asr_alignment_s);
}

size_t asr_runtime_workspace_bytes(void)
{
  return sizeof(union asr_runtime_workspace_u);
}

size_t asr_result_bytes(void)
{
  return sizeof(struct asr_result_s);
}
