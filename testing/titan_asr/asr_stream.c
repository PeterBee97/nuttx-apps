/****************************************************************************
 * asr_stream.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_AUDIOUTILS_TITAN_ASR_STREAM

#include <errno.h>
#include <math.h>
#include <string.h>

#include "asr_stream.h"

static void stream_clear(struct asr_stream_s *state)
{
  titan_intent_cancel(&state->gate);
  voicefe_features_init(&state->features);
  memset(state->ring, VOICEFE_FEATURE_SILENCE, sizeof(state->ring));
  state->rows = 0;
  state->quiet = 0;
  state->wake_blocks = 0;
  state->evaluated = false;
}

static void stream_event(struct asr_stream_s *state,
                         const struct titan_intent_event_s *event)
{
  if (event->kind == TITAN_EVENT_WAKE_INLINE ||
      event->kind == TITAN_EVENT_WAKE_WINDOW)
    {
      state->wake_events[event->kind - TITAN_EVENT_WAKE_INLINE]++;
    }
  else if (event->kind == TITAN_EVENT_INTENT &&
           event->result != TITAN_RESULT_COOLDOWN &&
           event->intent < TITAN_INTENT_ACTIONS)
    {
      state->actions[event->intent]++;
    }

  if (state->config.event != NULL)
    {
      state->config.event(event, event->kind == TITAN_EVENT_INTENT ?
                          &state->transcript : NULL,
                          state->config.argument);
    }
}

static int stream_copy(struct asr_stream_s *state, uint64_t first,
                        unsigned int count, bool wake)
{
  unsigned int capacity = wake ? VOICEFE_KWS_ROWS : VOICEFE_COMMAND_ROWS;
  unsigned int i;

  if (count > capacity || first > state->rows ||
      count > state->rows - first ||
      (state->rows > ASR_STREAM_RING_ROWS &&
       first < state->rows - ASR_STREAM_RING_ROWS))
    {
      state->snapshots_rejected++;
      return -ENOSPC;
    }

  memset(state->snapshot, VOICEFE_FEATURE_SILENCE,
         capacity * VOICEFE_MELS);
  for (i = 0; i < count; i++)
    {
      memcpy(state->snapshot + i * VOICEFE_MELS,
             state->ring[(first + i) % ASR_STREAM_RING_ROWS],
             VOICEFE_MELS);
    }

  return 0;
}

static int stream_invoke(struct asr_stream_s *state, bool wake,
                          struct titan_intent_score_s *score)
{
  unsigned int capacity = wake ? VOICEFE_KWS_ROWS : VOICEFE_COMMAND_ROWS;
  unsigned int frame;
  int length;
  int ret;

  ret = state->config.model(wake, state->snapshot,
                            capacity * VOICEFE_MELS, state->logits,
                            wake ? TITAN_WAKE_CLASSES :
                            ASR_STREAM_OUTPUT_BYTES,
                            state->config.argument);
  if (ret < 0)
    {
      return ret;
    }

  if (wake)
    {
      state->wake_invokes++;
      titan_intent_score(state->logits, TITAN_WAKE_CLASSES,
                        state->config.wake_scale_q20, score);
      return 0;
    }

  state->asr_invokes++;
  ret = asr_ctc_start(&state->workspace.decoder, &state->lm,
                      state->config.alpha, state->config.insertion_bonus,
                      state->config.blank_penalty);
  if (ret < 0)
    {
      return ret;
    }

  for (frame = 0; frame < ASR_CTC_FRAMES; frame++)
    {
      ret = asr_ctc_frame_int8(&state->workspace.decoder,
                              state->logits + frame * ASR_CTC_CLASSES,
                              state->config.asr_scale,
                              state->config.asr_zero);
      if (ret < 0)
        {
          return ret;
        }
    }

  memset(&state->transcript, 0, sizeof(state->transcript));
  length = asr_ctc_finish(&state->workspace.decoder,
                          state->transcript.tokens, ASR_CTC_FRAMES, NULL);
  if (length < 0)
    {
      return length;
    }

  state->transcript.length = length;
  ret = asr_postprocess_int8(&state->workspace.alignment, state->logits,
                             ASR_CTC_FRAMES, state->config.asr_scale,
                             state->config.asr_zero, &state->transcript,
                             state->config.threshold);
  if (ret < 0)
    {
      return ret;
    }

  memset(score, 0, sizeof(*score));
  score->label = state->transcript.intent == ASR_SILENCE ?
                 TITAN_SILENCE : TITAN_UNKNOWN;
  if (state->transcript.intent >= ASR_PHOTO_TAKE &&
      state->transcript.intent <= ASR_STOP_RECENT)
    {
      score->label = state->transcript.intent - ASR_PHOTO_TAKE;
      score->score = (uint16_t)lround(
        fmin(1.0, fmax(0.0, state->transcript.confidence)) * 32768);
    }

  return 0;
}

size_t asr_stream_bytes(void)
{
  return sizeof(struct asr_stream_s);
}

int asr_stream_init(struct asr_stream_s *state,
                    const struct asr_stream_config_s *config,
                    const void *lm_bytes, size_t lm_length,
                    uint32_t generation, bool companion)
{
  int ret;

  if (state == NULL || config == NULL || config->model == NULL ||
      config->wake_scale_q20 == 0 || generation == 0 ||
      !isfinite(config->asr_scale) || config->asr_scale <= 0 ||
      config->asr_zero < -128 || config->asr_zero > 127 ||
      !isfinite(config->threshold) ||
      config->threshold < 0 || config->threshold > 1 ||
      !isfinite(config->alpha) || !isfinite(config->insertion_bonus) ||
      !isfinite(config->blank_penalty) ||
      config->policy.command_score != 0 ||
      config->policy.command_margin != 0)
    {
      return -EINVAL;
    }

  memset(state, 0, sizeof(*state));
  state->config = *config;
  ret = asr_lm_open(&state->lm, lm_bytes, lm_length);
  if (ret < 0)
    {
      return ret;
    }

  titan_intent_init(&state->gate, &config->policy, generation);
  state->gate.state.active[TITAN_MODE_COMPANION] = companion;
  voicefe_dsp_init(&state->dsp, VOICEFE_LEFT, 32768, 32768);
  stream_clear(state);
  return 0;
}

int asr_stream_block(struct asr_stream_s *state,
                     const struct titan_intent_block_s *block)
{
  struct titan_intent_score_s score;
  struct titan_intent_event_s event;
  int8_t row[VOICEFE_MELS];
  uint64_t now;
  uint64_t first;
  uint64_t duration;
  unsigned int count;
  int ret;

  if (state == NULL || block == NULL ||
      block->generation != state->gate.state.generation)
    {
      return -EINVAL;
    }

  if (state->sequence_valid && block->sequence != state->expected)
    {
      state->skipped_turn = block->utterance_id;
      stream_clear(state);
    }

  state->sequence_valid = true;
  state->expected = block->sequence + 1;
  now = (uint64_t)block->sequence * 10;
  if (voicefe_features_push(&state->features, block->pcm, row))
    {
      memcpy(state->ring[state->rows++ % ASR_STREAM_RING_ROWS],
             row, sizeof(row));
    }

  if (block->sequence < 50)
    {
      return 0;
    }

  if (block->utterance_id != 0 &&
      block->utterance_id != state->skipped_turn &&
      !state->gate.in_turn &&
      titan_intent_begin(&state->gate, block->utterance_id,
                         block->start_sample / 16, block->generation))
    {
      state->turn_row = state->rows > ASR_STREAM_PREROLL_ROWS ?
                        state->rows - ASR_STREAM_PREROLL_ROWS : 0;
#ifdef CONFIG_TESTING_TITAN_ASR_VAD_START_BLOCKS
      duration = ((uint64_t)block->sequence * VOICEFE_FRAMES -
                  block->start_sample) / (2 * VOICEFE_FRAMES);
      state->turn_row = state->turn_row > duration ?
                        state->turn_row - duration : 0;
#endif
      state->quiet = 0;
      state->evaluated = false;
      state->last_speech_sample = block->start_sample;
    }

  if (state->gate.in_turn && block->utterance_id == state->gate.current)
    {
      if (block->speech)
        {
          state->last_speech_sample =
            (uint64_t)(block->sequence + 1) * VOICEFE_FRAMES;
          state->quiet = 0;
          if (!state->gate.emitted)
            {
              state->evaluated = false;
            }
        }
      else
        {
          state->quiet++;
        }

      duration = state->last_speech_sample - block->start_sample;
      if (!state->evaluated && state->quiet >= ASR_STREAM_END_BLOCKS)
        {
          memset(&score, 0, sizeof(score));
          score.label = TITAN_UNKNOWN;
          state->evaluated = true;
          first = state->turn_row;
          count = state->rows - first;
          if (count > VOICEFE_COMMAND_ROWS)
            {
              count -= state->quiet / 2;
              if (count > VOICEFE_COMMAND_ROWS)
                {
                  first += count - VOICEFE_COMMAND_ROWS;
                  count = VOICEFE_COMMAND_ROWS;
                }
            }

          if (duration > 64000)
            {
              titan_intent_candidate(&state->gate, block->utterance_id,
                                      block->generation, now, &score,
                                      true, &event);
            }
          else
            {
              ret = stream_copy(state, first, count, false);
              if (ret == 0)
                {
                  ret = stream_invoke(state, false, &score);
                }

              if (ret < 0)
                {
                  goto failed;
                }

              if (titan_intent_candidate(&state->gate, block->utterance_id,
                                         block->generation, now, &score,
                                         false, &event))
                {
                  stream_event(state, &event);
                }
            }
        }
    }

  if (++state->wake_blocks >= 10)
    {
      state->wake_blocks = 0;
      first = state->rows > VOICEFE_KWS_ROWS ?
              state->rows - VOICEFE_KWS_ROWS : 0;
      count = state->rows - first;
      ret = stream_copy(state, first, count, true);
      if (ret == 0)
        {
          ret = stream_invoke(state, true, &score);
        }

      if (ret < 0)
        {
          goto failed;
        }

      titan_intent_wake(&state->gate, &score);
    }

  if (block->marker_end &&
      titan_intent_end(&state->gate, block->utterance_id, now, &event))
    {
      stream_event(state, &event);
    }

  return 0;

failed:
  state->failures++;
  state->skipped_turn = block->utterance_id;
  stream_clear(state);
  return ret;
}

int asr_stream_feed(struct asr_stream_s *state,
                    const int16_t *pcm, size_t samples)
{
  struct titan_intent_block_s block;
  int16_t stereo[VOICEFE_FRAMES * 2];
  size_t offset;
  unsigned int i;
  int ret;

  if (state == NULL || pcm == NULL || samples % VOICEFE_FRAMES != 0)
    {
      return -EINVAL;
    }

  for (offset = 0; offset < samples; offset += VOICEFE_FRAMES)
    {
      memset(&block, 0, sizeof(block));
      for (i = 0; i < VOICEFE_FRAMES; i++)
        {
          stereo[2 * i] = pcm[offset + i];
          stereo[2 * i + 1] = pcm[offset + i];
        }

      voicefe_process(&state->dsp, stereo, block.pcm);
      block.speech = voicefe_speech(&state->dsp, block.pcm);
#ifdef CONFIG_TESTING_TITAN_ASR_VAD_START_BLOCKS
      if (state->marker == 0)
        {
          if (!block.speech)
            {
              state->marker_speech = 0;
            }
          else
            {
              if (state->marker_speech++ == 0)
                {
                  state->marker_start =
                    (uint64_t)state->sequence * VOICEFE_FRAMES;
                }

              if (state->marker_speech >=
                  CONFIG_TESTING_TITAN_ASR_VAD_START_BLOCKS)
                {
                  state->marker = ++state->marker_count;
                  state->marker_speech = 0;
                }
            }
        }
#else
      if (block.speech && state->marker == 0)
        {
          state->marker = ++state->marker_count;
          state->marker_start =
            (uint64_t)state->sequence * VOICEFE_FRAMES;
        }
#endif

      state->marker_quiet = block.speech ? 0 : state->marker_quiet + 1;
      block.marker_end = state->marker != 0 &&
                         state->marker_quiet >= ASR_STREAM_MARKER_END;
      block.sequence = state->sequence++;
      block.generation = state->gate.state.generation;
      block.utterance_id = state->marker;
      block.start_sample = state->marker_start;
      ret = asr_stream_block(state, &block);
      if (ret < 0)
        {
          return ret;
        }

      if (block.marker_end)
        {
          state->marker = 0;
        }
    }

  return 0;
}

void asr_stream_authorize_window(struct asr_stream_s *state)
{
  state->gate.state.wake_deadline =
    (uint64_t)state->sequence * 10 + state->gate.policy.window_ms;
}

void asr_stream_companion(struct asr_stream_s *state, bool enabled)
{
  state->gate.state.active[TITAN_MODE_COMPANION] = enabled;
}

void asr_stream_counts(const struct asr_stream_s *state, uint64_t *values)
{
  unsigned int i;

  values[0] = state->wake_events[0];
  values[1] = state->wake_events[1];
  for (i = 0; i < TITAN_INTENT_ACTIONS; i++)
    {
      values[2 + i] = state->actions[i];
    }

  values[10] = state->wake_invokes;
  values[11] = state->asr_invokes;
  values[12] = state->failures;
  values[13] = state->snapshots_rejected;
  values[14] = state->gate.state.too_long;
  values[15] = state->gate.state.denied;
  values[16] = state->marker_count;
}

#endif /* CONFIG_AUDIOUTILS_TITAN_ASR_STREAM */
