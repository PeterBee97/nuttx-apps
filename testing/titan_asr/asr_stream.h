/****************************************************************************
 * apps/testing/titan_asr/asr_stream.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef ASR_STREAM_H
#define ASR_STREAM_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <audioutils/titan_intent.h>
#include <audioutils/titan_voicefe.h>

#include "asr_runtime.h"

#ifdef CONFIG_TESTING_TITAN_ASR_PREROLL_ROWS
#define ASR_STREAM_PREROLL_ROWS CONFIG_TESTING_TITAN_ASR_PREROLL_ROWS
#else
#define ASR_STREAM_PREROLL_ROWS 15
#endif
#ifdef CONFIG_TESTING_TITAN_ASR_END_BLOCKS
#define ASR_STREAM_END_BLOCKS   CONFIG_TESTING_TITAN_ASR_END_BLOCKS
#else
#define ASR_STREAM_END_BLOCKS   25
#endif
#ifdef CONFIG_TESTING_TITAN_ASR_MARKER_END
#define ASR_STREAM_MARKER_END CONFIG_TESTING_TITAN_ASR_MARKER_END
#else
#define ASR_STREAM_MARKER_END 80
#endif
#define ASR_STREAM_RING_ROWS    (199 + 15 + 25 / 2 + 1)
#define ASR_STREAM_OUTPUT_BYTES (ASR_CTC_FRAMES * ASR_CTC_CLASSES)

typedef int (*asr_stream_model_t)(bool wake, const int8_t *features,
                                  size_t feature_bytes, int8_t *output,
                                  size_t output_bytes, void *argument);
typedef void (*asr_stream_event_t)(
  const struct titan_intent_event_s *event,
  const struct asr_result_s *transcript, void *argument);

struct asr_stream_config_s
{
  struct titan_intent_policy_s policy;
  double alpha;
  double insertion_bonus;
  double blank_penalty;
  double threshold;
  float asr_scale;
  int asr_zero;
  uint32_t wake_scale_q20;
  asr_stream_model_t model;
  asr_stream_event_t event;
  void *argument;
};

struct asr_stream_s
{
  struct asr_stream_config_s config;
  struct titan_intent_s gate;
  struct voicefe_features_s features;
  struct voicefe_dsp_s dsp;
  struct asr_lm_s lm;
  union asr_runtime_workspace_u workspace;
  struct asr_result_s transcript;
  int8_t ring[ASR_STREAM_RING_ROWS][VOICEFE_MELS];
  int8_t snapshot[VOICEFE_COMMAND_ROWS * VOICEFE_MELS];
  int8_t logits[ASR_STREAM_OUTPUT_BYTES];
  uint64_t wake_events[2];
  uint64_t actions[TITAN_INTENT_ACTIONS];
  uint64_t wake_invokes;
  uint64_t asr_invokes;
  uint64_t failures;
  uint64_t snapshots_rejected;
  uint64_t rows;
  uint64_t turn_row;
  uint64_t last_speech_sample;
  uint64_t skipped_turn;
  uint64_t marker;
  uint64_t marker_count;
  uint64_t marker_start;
  uint32_t sequence;
  uint32_t expected;
  unsigned int quiet;
  unsigned int marker_quiet;
#ifdef CONFIG_TESTING_TITAN_ASR_VAD_START_BLOCKS
  unsigned int marker_speech;
#endif
#if CONFIG_TESTING_TITAN_ASR_VAD_ONSET_HOLD > 0
  unsigned int onset_quiet;
  bool onset_pending;
#endif
  unsigned int wake_blocks;
  bool sequence_valid;
  bool evaluated;
};

size_t asr_stream_bytes(void);
int asr_stream_init(struct asr_stream_s *state,
                    const struct asr_stream_config_s *config,
                    const void *lm_bytes, size_t lm_length,
                    uint32_t generation, bool companion);
int asr_stream_block(struct asr_stream_s *state,
                     const struct titan_intent_block_s *block);
int asr_stream_feed(struct asr_stream_s *state,
                    const int16_t *pcm, size_t samples);
void asr_stream_authorize_window(struct asr_stream_s *state);
void asr_stream_companion(struct asr_stream_s *state, bool enabled);
void asr_stream_counts(const struct asr_stream_s *state, uint64_t *values);

#endif /* ASR_STREAM_H */
