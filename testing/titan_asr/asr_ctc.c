/****************************************************************************
 * apps/testing/titan_asr/asr_ctc.c
 *
 * Streaming CTC prefix search. Characters have unrestricted transitions;
 * training-only character probabilities do not define an action whitelist.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "asr_ctc.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#include "asr_math.h"
#ifdef CONFIG_TESTING_TITAN_ASR_MVE
#  include "asr_mve.h"
#endif
#ifdef CONFIG_TESTING_TITAN_ASR_OPT
#  include <nuttx/clock.h>
#  include "asr_opt.h"

unsigned int g_asr_opt_mask;
bool g_asr_profile_enabled;
uint64_t g_asr_profile_cycles[ASR_PROFILE_COUNT];
uint32_t g_asr_profile_calls[ASR_PROFILE_COUNT];

static uint32_t profile_start(void)
{
#ifdef CONFIG_TESTING_TITAN_ASR_PROFILE
  return g_asr_profile_enabled ? (uint32_t)perf_gettime() : 0;
#else
  return 0;
#endif
}

static void profile_end(unsigned int section, uint32_t start)
{
#ifdef CONFIG_TESTING_TITAN_ASR_PROFILE
  if (g_asr_profile_enabled)
    {
      g_asr_profile_cycles[section] += (uint32_t)perf_gettime() - start;
      g_asr_profile_calls[section]++;
    }
#endif
}
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint16_t read16(const uint8_t *ptr)
{
  return (uint16_t)ptr[0] | ((uint16_t)ptr[1] << 8);
}

static uint32_t read32(const uint8_t *ptr)
{
  return (uint32_t)ptr[0] | ((uint32_t)ptr[1] << 8) |
         ((uint32_t)ptr[2] << 16) | ((uint32_t)ptr[3] << 24);
}

static double logadd_impl(double left, double right)
{
  double high;
  double low;

  if (left == -INFINITY)
    {
      return right;
    }

  if (right == -INFINITY)
    {
      return left;
    }

  high = left > right ? left : right;
  low = left > right ? right : left;
  return high + log1p(exp(low - high));
}

static double logadd(double left, double right)
{
#ifdef CONFIG_TESTING_TITAN_ASR_OPT
  uint32_t start = profile_start();
#endif
  double result = logadd_impl(left, right);
#ifdef CONFIG_TESTING_TITAN_ASR_OPT
  profile_end(ASR_PROFILE_LOGADD, start);
#endif
  return result;
}

static int key_compare(const uint8_t *row, const uint16_t *key,
                       unsigned int fields)
{
  unsigned int i;
  uint16_t value;

  for (i = 0; i < fields; i++)
    {
      value = read16(row + 2 * i);
      if (value != key[i])
        {
          return value < key[i] ? -1 : 1;
        }
    }

  return 0;
}

static int table_lookup(const struct asr_lm_s *lm, unsigned int table,
                        const uint16_t *key, unsigned int fields)
{
  const uint8_t *row;
  uint32_t left = 0;
  uint32_t right = lm->counts[table];
  uint32_t middle;
  unsigned int stride = 2 * (fields + 1);
  int comparison;

  while (left < right)
    {
      middle = left + (right - left) / 2;
      row = lm->tables[table] + middle * stride;
      comparison = key_compare(row, key, fields);
      if (comparison == 0)
        {
          return read16(row + 2 * fields);
        }

      if (comparison < 0)
        {
          left = middle + 1;
        }
      else
        {
          right = middle;
        }
    }

  return -1;
}

int asr_lm_open(struct asr_lm_s *lm, const void *bytes, size_t length)
{
  static const unsigned int fields[5] =
  {
    1, 2, 3, 1, 2
  };

  struct asr_lm_s candidate;
  const uint8_t *blob = bytes;
  const uint8_t *previous;
  const uint8_t *row;
  uint16_t key[3];
  size_t offset = 44;
  size_t stride;
  uint32_t i;
  unsigned int bank;
  unsigned int field;

  if (lm == NULL || bytes == NULL || length < offset ||
      memcmp(blob, "VFLM0001", 8) != 0)
    {
      return -EINVAL;
    }

  memset(&candidate, 0, sizeof(candidate));
  candidate.classes = read32(blob + 8);
  candidate.bos = read32(blob + 12);
  candidate.eos = read32(blob + 16);
  candidate.scale = read32(blob + 20);
  if (candidate.classes != ASR_CTC_CLASSES ||
      candidate.bos != candidate.classes ||
      candidate.eos != candidate.classes + 1 || candidate.scale == 0)
    {
      return -EINVAL;
    }

  for (bank = 0; bank < 5; bank++)
    {
      candidate.counts[bank] = read32(blob + 24 + 4 * bank);
      stride = 2 * (fields[bank] + 1);
      if (candidate.counts[bank] > (length - offset) / stride)
        {
          return -EINVAL;
        }

      candidate.tables[bank] = blob + offset;
      previous = NULL;
      for (i = 0; i < candidate.counts[bank]; i++)
        {
          row = blob + offset + i * stride;
          for (field = 0; field < fields[bank]; field++)
            {
              key[field] = read16(row + 2 * field);
              if (key[field] > candidate.eos)
                {
                  return -EINVAL;
                }
            }

          if (previous != NULL &&
              key_compare(previous, key, fields[bank]) >= 0)
            {
              return -EINVAL;
            }

          previous = row;
        }

      offset += candidate.counts[bank] * stride;
    }

  if (offset != length)
    {
      return -EINVAL;
    }

  /* Exported unigram probabilities cover every nonblank token and EOS.
   * Missing probabilities are rejected instead of silently restricting
   * the character vocabulary.
   */

  for (i = 1; i <= candidate.eos; i++)
    {
      if (i == candidate.bos)
        {
          continue;
        }

      key[0] = (uint16_t)i;
      if (table_lookup(&candidate, 0, key, 1) < 0)
        {
          return -EINVAL;
        }
    }

  *lm = candidate;
  return 0;
}

static double lm_logp_impl(const struct asr_lm_s *lm, uint16_t first,
                   uint16_t second, uint16_t token)
{
  uint16_t key[3];
  int value;
  int direct;
  int backoff;

#ifdef CONFIG_TESTING_TITAN_ASR_OPT
  if ((g_asr_opt_mask & ASR_OPT_LM) != 0)
    {
      /* A higher order direct probability replaces lower order terms.
       * Avoid computing terms that would immediately be overwritten.
       */

      key[0] = first;
      key[1] = second;
      key[2] = token;
      direct = table_lookup(lm, 2, key, 3);
      if (direct >= 0)
        {
          return -(double)direct / lm->scale;
        }

      backoff = table_lookup(lm, 4, key, 2);
      value = backoff < 0 ? 0 : backoff;
      key[0] = second;
      key[1] = token;
      direct = table_lookup(lm, 1, key, 2);
      if (direct >= 0)
        {
          return -(double)(value + direct) / lm->scale;
        }

      backoff = table_lookup(lm, 3, key, 1);
      value += backoff < 0 ? 0 : backoff;
      key[0] = token;
      direct = table_lookup(lm, 0, key, 1);
      return direct < 0 ? -INFINITY :
             -(double)(value + direct) / lm->scale;
    }
#endif

  key[0] = token;
  value = table_lookup(lm, 0, key, 1);
  if (value < 0)
    {
      return -INFINITY;
    }

  key[0] = second;
  key[1] = token;
  direct = table_lookup(lm, 1, key, 2);
  if (direct >= 0)
    {
      value = direct;
    }
  else
    {
      backoff = table_lookup(lm, 3, key, 1);
      value += backoff < 0 ? 0 : backoff;
    }

  key[0] = first;
  key[1] = second;
  key[2] = token;
  direct = table_lookup(lm, 2, key, 3);
  if (direct >= 0)
    {
      value = direct;
    }
  else
    {
      backoff = table_lookup(lm, 4, key, 2);
      value += backoff < 0 ? 0 : backoff;
    }

  return -(double)value / lm->scale;
}

double asr_lm_logp(const struct asr_lm_s *lm, uint16_t first,
                   uint16_t second, uint16_t token)
{
#ifdef CONFIG_TESTING_TITAN_ASR_OPT
  uint32_t start = profile_start();
#endif
  double result = lm_logp_impl(lm, first, second, token);
#ifdef CONFIG_TESTING_TITAN_ASR_OPT
  profile_end(ASR_PROFILE_LM, start);
#endif
  return result;
}

static double next_language(const struct asr_ctc_s *state,
                            const struct asr_prefix_s *prefix,
                            uint16_t token, double *probability)
{
  uint16_t first;
  uint16_t second;
  double language = prefix->language + state->bonus;

  if (state->alpha != 0)
    {
      first = prefix->length >= 2 ? prefix->tokens[prefix->length - 2] :
              (uint16_t)state->lm->bos;
      second = prefix->length >= 1 ? prefix->tokens[prefix->length - 1] :
               (uint16_t)state->lm->bos;
      if (probability != NULL)
        {
          if (isnan(*probability))
            {
              *probability = asr_lm_logp(state->lm, first, second, token);
            }

          language += state->alpha * *probability;
        }
      else
        {
          language += state->alpha * asr_lm_logp(state->lm, first, second,
                                               token);
        }
    }

  return language;
}

static int candidate_impl(struct asr_ctc_s *state, unsigned int *count,
                         const struct asr_prefix_s *source, int token,
                         double language, double blank, double nonblank
#ifdef CONFIG_TESTING_TITAN_ASR_OPT
                         , uint16_t *slots
#endif
                         )
{
  struct asr_prefix_s *candidate;
  unsigned int length = source->length + (token > 0);
  unsigned int i;
  int match;
#ifdef CONFIG_TESTING_TITAN_ASR_OPT
  uint32_t hash = UINT32_C(2166136261);
  unsigned int slot = 0;

  if ((g_asr_opt_mask & ASR_OPT_HASH) != 0)
    {
      for (i = 0; i < source->length; i++)
        {
          hash = (hash ^ source->tokens[i]) * UINT32_C(16777619);
        }

      if (token > 0)
        {
          hash = (hash ^ token) * UINT32_C(16777619);
        }

      slot = hash & (ASR_HASH_SLOTS - 1);
    }
#endif

  if (length > ASR_CTC_FRAMES)
    {
      return -EOVERFLOW;
    }

  for (i = 0; i < *count; i++)
    {
#ifdef CONFIG_TESTING_TITAN_ASR_OPT
      if ((g_asr_opt_mask & ASR_OPT_HASH) != 0)
        {
          if (slots[slot] == 0)
            {
              break;
            }

          candidate = &state->candidates[slots[slot] - 1];
          slot = (slot + 1) & (ASR_HASH_SLOTS - 1);
        }
      else
#endif
        {
          candidate = &state->candidates[i];
        }

      match = candidate->length == length &&
              memcmp(candidate->tokens, source->tokens,
                     source->length * sizeof(uint16_t)) == 0;
      if (match && (token <= 0 ||
                    candidate->tokens[length - 1] == (uint16_t)token))
        {
          candidate->blank = logadd(candidate->blank, blank);
          candidate->nonblank = logadd(candidate->nonblank, nonblank);
          return 0;
        }
    }

  if (*count >= ASR_CTC_CANDIDATES)
    {
      return -EOVERFLOW;
    }

#ifdef CONFIG_TESTING_TITAN_ASR_OPT
  if ((g_asr_opt_mask & ASR_OPT_HASH) != 0)
    {
      slots[slot] = *count + 1;
    }

#endif
  candidate = &state->candidates[(*count)++];
  memcpy(candidate->tokens, source->tokens,
         source->length * sizeof(uint16_t));
  if (token > 0)
    {
      candidate->tokens[length - 1] = (uint16_t)token;
    }

  candidate->length = (uint16_t)length;
  candidate->blank = blank;
  candidate->nonblank = nonblank;
  candidate->language = language;
  return 0;
}

static int add_candidate(struct asr_ctc_s *state, unsigned int *count,
                         const struct asr_prefix_s *source, int token,
                         double language, double blank, double nonblank
#ifdef CONFIG_TESTING_TITAN_ASR_OPT
                         , uint16_t *slots
#endif
                         )
{
#ifdef CONFIG_TESTING_TITAN_ASR_OPT
  uint32_t start = profile_start();
#endif
  int result = candidate_impl(state, count, source, token, language,
                              blank, nonblank
#ifdef CONFIG_TESTING_TITAN_ASR_OPT
                              , slots
#endif
                              );
#ifdef CONFIG_TESTING_TITAN_ASR_OPT
  profile_end(ASR_PROFILE_MERGE, start);
#endif
  return result;
}

#ifdef CONFIG_TESTING_TITAN_ASR_OPT
#  define ADD_CANDIDATE(...) add_candidate(__VA_ARGS__, slots)
#else
#  define ADD_CANDIDATE(...) add_candidate(__VA_ARGS__)
#endif

int asr_ctc_start(struct asr_ctc_s *state, const struct asr_lm_s *lm,
                  double alpha, double bonus, double blank_penalty)
{
  if (state == NULL || !isfinite(alpha) || !isfinite(bonus) ||
      !isfinite(blank_penalty) || alpha < 0 ||
      (alpha != 0 && lm == NULL))
    {
      return -EINVAL;
    }

  memset(state, 0, sizeof(*state));
  state->nbeams = 1;
  state->beams[0].nonblank = -INFINITY;
  state->alpha = alpha;
  state->bonus = bonus;
  state->blank_penalty = blank_penalty;
  state->lm = lm;
  return 0;
}

int asr_ctc_frame(struct asr_ctc_s *state, const float *logits)
{
#ifdef CONFIG_TESTING_TITAN_ASR_OPT
  uint16_t slots[ASR_HASH_SLOTS];
  uint32_t profile;

  if ((g_asr_opt_mask & ASR_OPT_HASH) != 0)
    {
      memset(slots, 0, sizeof(slots));
    }

  profile = profile_start();
#endif
#ifdef CONFIG_TESTING_TITAN_ASR_LM_CACHE
  double probabilities[ASR_CTC_BEAMS][ASR_CTC_FRAME_TOKENS];
  uint16_t histories[ASR_CTC_BEAMS][2];
  unsigned int banks[ASR_CTC_BEAMS];
  unsigned int nbanks = 0;
  unsigned int bank;
  uint16_t first;
  uint16_t second;
#endif
  double frame[ASR_CTC_CLASSES];
#ifdef CONFIG_TESTING_TITAN_ASR_MVE
  float exponentials[ASR_CTC_CLASSES];
#endif
  uint16_t order[ASR_CTC_FRAME_TOKENS];
  unsigned int retained[ASR_CTC_BEAMS];
  struct asr_prefix_s *prefix;
  double maximum = -INFINITY;
  double sum = 0;
  double normalizer;
  double total;
  double acoustic;
  double language;
  unsigned int count = 0;
  unsigned int norder = 0;
  unsigned int nretained = 0;
  unsigned int position;
  unsigned int i;
  unsigned int j;
  uint16_t token;
  int result;

  if (state == NULL || logits == NULL || state->nbeams == 0 ||
      state->frames >= ASR_CTC_FRAMES)
    {
      return -EINVAL;
    }

  for (i = 0; i < ASR_CTC_CLASSES; i++)
    {
      if (!isfinite(logits[i]))
        {
          return -EINVAL;
        }

      if (logits[i] > maximum)
        {
          maximum = logits[i];
        }
    }

#ifdef CONFIG_TESTING_TITAN_ASR_MVE
  if (asr_mve_enabled())
    {
      asr_mve_exponentials(logits, maximum, exponentials, ASR_CTC_CLASSES);
    }
#endif

  for (i = 0; i < ASR_CTC_CLASSES; i++)
    {
      frame[i] = (double)logits[i] - maximum;
#ifdef CONFIG_TESTING_TITAN_ASR_MVE
      if (asr_mve_enabled())
        {
          sum += exponentials[i];
        }
      else
#endif
        {
          sum += exp(frame[i]);
        }
    }

  normalizer = log(sum);
  for (i = 0; i < ASR_CTC_CLASSES; i++)
    {
      frame[i] -= normalizer;
    }

  frame[0] -= state->blank_penalty;
  for (i = 0; i < ASR_CTC_CLASSES; i++)
    {
#ifdef CONFIG_TESTING_TITAN_ASR_OPT
      if ((g_asr_opt_mask & ASR_OPT_TOPK) != 0 &&
          norder == ASR_CTC_FRAME_TOKENS &&
          frame[i] <= frame[order[norder - 1]])
        {
          continue;
        }

#endif
      position = 0;
      while (position < norder && frame[order[position]] >= frame[i])
        {
          position++;
        }

      if (position >= ASR_CTC_FRAME_TOKENS)
        {
          continue;
        }

      if (norder < ASR_CTC_FRAME_TOKENS)
        {
          norder++;
        }

      for (j = norder - 1; j > position; j--)
        {
          order[j] = order[j - 1];
        }

      order[position] = (uint16_t)i;
    }

#ifdef CONFIG_TESTING_TITAN_ASR_OPT
  profile_end(ASR_PROFILE_SETUP, profile);
#endif

  for (i = 0; i < state->nbeams; i++)
    {
      prefix = &state->beams[i];
#ifdef CONFIG_TESTING_TITAN_ASR_LM_CACHE
      if (state->alpha != 0)
        {
          first = prefix->length >= 2 ?
                  prefix->tokens[prefix->length - 2] :
                  (uint16_t)state->lm->bos;
          second = prefix->length >= 1 ?
                   prefix->tokens[prefix->length - 1] :
                   (uint16_t)state->lm->bos;
          for (bank = 0; bank < nbanks; bank++)
            {
              if (histories[bank][0] == first &&
                  histories[bank][1] == second)
                {
                  break;
                }
            }

          if (bank == nbanks)
            {
              histories[bank][0] = first;
              histories[bank][1] = second;
              for (j = 0; j < norder; j++)
                {
                  probabilities[bank][j] = NAN;
                }

              nbanks++;
            }

          banks[i] = bank;
        }
#endif

      total = logadd(prefix->blank, prefix->nonblank);
      result = ADD_CANDIDATE(state, &count, prefix, 0, prefix->language,
                             total + frame[0], -INFINITY);
      if (result < 0)
        {
          return result;
        }

      /* Python explores retained acoustic tokens in increasing score order.
       * Match that order so prefix ties retain the same insertion order.
       */

      for (j = norder; j > 0; j--)
        {
          token = order[j - 1];
          if (token == 0)
            {
              continue;
            }

#ifdef CONFIG_TESTING_TITAN_ASR_LM_CACHE
          language = next_language(state, prefix, token,
                                   state->alpha != 0 ?
                                   &probabilities[banks[i]][j - 1] : NULL);
#else
          language = next_language(state, prefix, token, NULL);
#endif
          acoustic = total;
          if (prefix->length && token == prefix->tokens[prefix->length - 1])
            {
              result = ADD_CANDIDATE(state, &count, prefix, 0,
                                     prefix->language, -INFINITY,
                                     prefix->nonblank + frame[token]);
              if (result < 0)
                {
                  return result;
                }

              acoustic = prefix->blank;
            }

          result = ADD_CANDIDATE(state, &count, prefix, token, language,
                                 -INFINITY, acoustic + frame[token]);
          if (result < 0)
            {
              return result;
            }
        }
    }

#ifdef CONFIG_TESTING_TITAN_ASR_OPT
  profile = profile_start();
#endif

  for (i = 0; i < count; i++)
    {
      prefix = &state->candidates[i];
      prefix->rank = logadd(prefix->blank, prefix->nonblank) +
                     prefix->language;
#ifdef CONFIG_TESTING_TITAN_ASR_OPT
      if ((g_asr_opt_mask & ASR_OPT_TOPK) != 0 &&
          nretained == ASR_CTC_BEAMS &&
          prefix->rank <= state->candidates[retained[nretained - 1]].rank)
        {
          continue;
        }

#endif
      position = 0;
      while (position < nretained &&
             state->candidates[retained[position]].rank >= prefix->rank)
        {
          position++;
        }

      if (position >= ASR_CTC_BEAMS)
        {
          continue;
        }

      if (nretained < ASR_CTC_BEAMS)
        {
          nretained++;
        }

      for (j = nretained - 1; j > position; j--)
        {
          retained[j] = retained[j - 1];
        }

      retained[position] = i;
    }

  for (i = 0; i < nretained; i++)
    {
      state->beams[i] = state->candidates[retained[i]];
    }

  state->nbeams = nretained;
  state->frames++;
#ifdef CONFIG_TESTING_TITAN_ASR_OPT
  profile_end(ASR_PROFILE_RANK, profile);
#endif
  return 0;
}

int asr_ctc_finish(const struct asr_ctc_s *state, uint16_t *tokens,
                   size_t capacity, double *acoustic_log_probability)
{
  const struct asr_prefix_s *prefix;
  unsigned int selected = 0;
  unsigned int i;
  double best = -INFINITY;
  double rank;
  uint16_t first;
  uint16_t second;

  if (state == NULL || tokens == NULL || state->frames == 0 ||
      state->nbeams == 0)
    {
      return -EINVAL;
    }

  for (i = 0; i < state->nbeams; i++)
    {
      prefix = &state->beams[i];
      rank = logadd(prefix->blank, prefix->nonblank) + prefix->language;
      if (state->alpha != 0)
        {
          first = prefix->length >= 2 ?
                  prefix->tokens[prefix->length - 2] :
                  (uint16_t)state->lm->bos;
          second = prefix->length >= 1 ?
                   prefix->tokens[prefix->length - 1] :
                   (uint16_t)state->lm->bos;
          rank += state->alpha * asr_lm_logp(state->lm, first, second,
                                            (uint16_t)state->lm->eos);
        }

      if (rank > best)
        {
          best = rank;
          selected = i;
        }
    }

  prefix = &state->beams[selected];
  if (capacity < prefix->length)
    {
      return -ENOSPC;
    }

  memcpy(tokens, prefix->tokens, prefix->length * sizeof(uint16_t));
  if (acoustic_log_probability != NULL)
    {
      *acoustic_log_probability = logadd(prefix->blank, prefix->nonblank);
    }

  return prefix->length;
}

size_t asr_ctc_state_bytes(void)
{
  return sizeof(struct asr_ctc_s);
}
