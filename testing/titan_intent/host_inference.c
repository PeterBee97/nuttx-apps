/****************************************************************************
 * apps/testing/titan_intent/host_inference.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/* Run the deployed DSP, VAD and ML stream on host PCM. The supplied callback
 * runs the uncompiled INT8 model; this does not execute or simulate U55.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define pthread_setname_np(thread, name) (0)
#define sem_setprotocol(sem, protocol) (0)

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "../../system/titan_voice/voicefe_intent.c"

/****************************************************************************
 * Private Types
 ****************************************************************************/

typedef int (*host_model_t)(int wake, const int8_t *features, size_t bytes,
                            int8_t *logits);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static host_model_t g_model;
static uint32_t g_scale[2];
static struct voicefe_dsp_s g_dsp;
static uint32_t g_sequence;
static uint64_t g_marker;
static uint64_t g_marker_count;
static uint64_t g_marker_start;
static unsigned int g_marker_quiet;
static uint64_t g_host_wake[2];
static uint64_t g_host_actions[TITAN_INTENT_ACTIONS];
static uint64_t g_host_events;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int tflm_titan_intent_init(void)
{
  return 0;
}

void tflm_titan_intent_deinit(void)
{
}

int tflm_titan_intent_invoke(bool wake, const int8_t *features, size_t bytes,
                            struct titan_intent_score_s *score,
                            uint64_t *cycles, uint32_t *elapsed_us)
{
  int8_t logits[TITAN_INTENT_CLASSES];
  int ret;

  if (g_model == NULL)
    {
      return -ENOSYS;
    }

  ret = g_model(wake, features, bytes, logits);
  if (ret < 0)
    {
      return ret;
    }

  titan_intent_score(logits,
                    wake ? TITAN_WAKE_CLASSES : TITAN_INTENT_CLASSES,
                    g_scale[wake ? 0 : 1], score);
  *cycles = 0;
  *elapsed_us = 0;
  return 0;
}

size_t tflm_titan_intent_arena(bool wake)
{
  return 0;
}

const char *tflm_titan_intent_version(void)
{
  return "host-int8-no-u55";
}

void host_inference_init(host_model_t model, uint32_t wake_scale,
                         uint32_t command_scale, uint16_t wake_score,
                         uint16_t wake_margin, uint16_t command_score,
                         uint16_t command_margin, bool companion)
{
  struct titan_intent_policy_s policy =
  {
    8000, 1000, wake_score, wake_margin, command_score, command_margin
  };

  memset(&g_ml, 0, sizeof(g_ml));
  memset(g_host_wake, 0, sizeof(g_host_wake));
  memset(g_host_actions, 0, sizeof(g_host_actions));
  g_host_events = 0;
  g_sequence = 0;
  g_marker = 0;
  g_marker_count = 0;
  g_marker_start = 0;
  g_marker_quiet = 0;
  g_model = model;
  g_scale[0] = wake_scale;
  g_scale[1] = command_scale;
  voicefe_dsp_init(&g_dsp, VOICEFE_LEFT, 32768, 32768);
  titan_intent_init(&g_ml.gate, &policy, 1);
  g_ml.gate.state.active[TITAN_MODE_COMPANION] = companion;
  voicefe_ml_clear();
}

void host_inference_feed(const int16_t *pcm, size_t samples)
{
  struct titan_intent_block_s block;
  struct titan_intent_event_s event;
  int16_t stereo[VOICEFE_FRAMES * 2];
  unsigned int i;
  size_t offset;

  for (offset = 0; offset + VOICEFE_FRAMES <= samples;
       offset += VOICEFE_FRAMES)
    {
      memset(&block, 0, sizeof(block));
      for (i = 0; i < VOICEFE_FRAMES; i++)
        {
          stereo[2 * i] = pcm[offset + i];
          stereo[2 * i + 1] = pcm[offset + i];
        }

      voicefe_process(&g_dsp, stereo, block.pcm);
      block.speech = voicefe_speech(&g_dsp, block.pcm);
      if (block.speech && g_marker == 0)
        {
          g_marker = ++g_marker_count;
          g_marker_start = (uint64_t)g_sequence * VOICEFE_FRAMES;
        }

      g_marker_quiet = block.speech ? 0 : g_marker_quiet + 1;
      block.marker_end = g_marker != 0 && g_marker_quiet >= 80;
      block.sequence = g_sequence++;
      block.generation = 1;
      block.utterance_id = g_marker;
      block.start_sample = g_marker_start;
      voicefe_ml_block(&block);
      while (voicefe_intent_poll(&event))
        {
          g_host_events++;
          if (event.kind == TITAN_EVENT_WAKE_INLINE ||
              event.kind == TITAN_EVENT_WAKE_WINDOW)
            {
              g_host_wake[event.kind - TITAN_EVENT_WAKE_INLINE]++;
            }
          else if (event.kind == TITAN_EVENT_INTENT &&
                   event.result != TITAN_RESULT_COOLDOWN &&
                   event.intent < TITAN_INTENT_ACTIONS)
            {
              g_host_actions[event.intent]++;
            }
        }

      if (block.marker_end)
        {
          g_marker = 0;
        }
    }
}

void host_inference_counts(uint64_t *values)
{
  unsigned int i;

  values[0] = g_host_wake[0];
  values[1] = g_host_wake[1];
  for (i = 0; i < TITAN_INTENT_ACTIONS; i++)
    {
      values[i + 2] = g_host_actions[i];
    }

  values[10] = g_host_events;
  values[11] = g_ml.stats.wake_invokes;
  values[12] = g_ml.stats.command_invokes;
  values[13] = g_ml.gate.state.too_long;
  values[14] = g_ml.gate.state.denied;
  values[15] = g_marker_count;
}
