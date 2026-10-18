/****************************************************************************
 * asr_runtime.h
 *
 * Host precheck of full ASR confidence and independent text classification.
 * This interface has not been frozen or validated on a board.
 ****************************************************************************/

#ifndef ASR_RUNTIME_H
#define ASR_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "asr_ctc.h"

#define ASR_ALIGNMENT_STATES (2 * ASR_CTC_FRAMES + 1)
#define ASR_ALIGNMENT_STRIDE ((ASR_ALIGNMENT_STATES + 3) / 4)
#define ASR_TEXT_BYTES       (4 * ASR_CTC_FRAMES + 1)

enum asr_intent_e
{
  ASR_SILENCE = 0,
  ASR_UNKNOWN,
  ASR_PHOTO_TAKE,
  ASR_VIDEO_START,
  ASR_VIDEO_STOP,
  ASR_AUDIO_START,
  ASR_AUDIO_STOP,
  ASR_COMPANION_ENTER,
  ASR_COMPANION_EXIT,
  ASR_STOP_RECENT
};

struct asr_alignment_s
{
  double scores[2][ASR_ALIGNMENT_STATES];
  double normalizers[ASR_CTC_FRAMES];
  uint8_t back[ASR_CTC_FRAMES][ASR_ALIGNMENT_STRIDE];
};

/* Prefix search and alignment run sequentially and may share this storage. */

union asr_runtime_workspace_u
{
  struct asr_ctc_s decoder;
  struct asr_alignment_s alignment;
};

struct asr_result_s
{
  uint16_t tokens[ASR_CTC_FRAMES];
  uint16_t frames[ASR_CTC_FRAMES];
  float token_confidence[ASR_CTC_FRAMES];
  unsigned int length;
  unsigned int unknown_characters;
  double confidence;
  double minimum_confidence;
  enum asr_intent_e intent;
  char text[ASR_TEXT_BYTES];
};

int asr_text_classify(const char *text);
int asr_postprocess_float(struct asr_alignment_s *workspace,
                          const float *logits, unsigned int frames,
                          struct asr_result_s *result, double threshold);
int asr_postprocess_int8(struct asr_alignment_s *workspace,
                         const int8_t *logits, unsigned int frames,
                         float scale, int zero_point,
                         struct asr_result_s *result, double threshold);
int asr_ctc_frame_int8(struct asr_ctc_s *state, const int8_t *logits,
                      float scale, int zero_point);
size_t asr_alignment_bytes(void);
size_t asr_runtime_workspace_bytes(void);
size_t asr_result_bytes(void);

#endif /* ASR_RUNTIME_H */
