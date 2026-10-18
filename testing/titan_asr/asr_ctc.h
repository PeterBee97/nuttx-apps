/****************************************************************************
 * asr_ctc.h
 *
 * Host precheck of an unrestricted, bounded character CTC prefix decoder.
 * This is not a board validation or a frozen production interface.
 ****************************************************************************/

#ifndef ASR_CTC_H
#define ASR_CTC_H

#include <stddef.h>
#include <stdint.h>

#define ASR_CTC_FRAMES       100
#define ASR_CTC_CLASSES      303
#define ASR_CTC_BEAMS        8
#define ASR_CTC_FRAME_TOKENS 12
#define ASR_CTC_CANDIDATES   (ASR_CTC_BEAMS * (ASR_CTC_FRAME_TOKENS + 1))

struct asr_lm_s
{
  const uint8_t *tables[5];
  uint32_t counts[5];
  uint32_t classes;
  uint32_t bos;
  uint32_t eos;
  uint32_t scale;
};

struct asr_prefix_s
{
  uint16_t tokens[ASR_CTC_FRAMES];
  uint16_t length;
  double blank;
  double nonblank;
  double language;
  double rank;
};

struct asr_ctc_s
{
  struct asr_prefix_s beams[ASR_CTC_BEAMS];
  struct asr_prefix_s candidates[ASR_CTC_CANDIDATES];
  unsigned int nbeams;
  unsigned int frames;
  double alpha;
  double bonus;
  double blank_penalty;
  const struct asr_lm_s *lm;
};

/* LM bytes must remain available for the lifetime of the decoder. */

int asr_lm_open(struct asr_lm_s *lm, const void *bytes, size_t length);
double asr_lm_logp(const struct asr_lm_s *lm, uint16_t first,
                   uint16_t second, uint16_t token);
int asr_ctc_start(struct asr_ctc_s *state, const struct asr_lm_s *lm,
                  double alpha, double bonus, double blank_penalty);
int asr_ctc_frame(struct asr_ctc_s *state, const float *logits);
int asr_ctc_finish(const struct asr_ctc_s *state, uint16_t *tokens,
                   size_t capacity, double *acoustic_log_probability);
size_t asr_ctc_state_bytes(void);

#endif /* ASR_CTC_H */
