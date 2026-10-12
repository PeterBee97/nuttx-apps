/****************************************************************************
 * apps/testing/titan_intent/host_stream.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/* Exercise the deployed streaming code with scripted model output. This
 * tests transport, endpoint and failure semantics, not model recognition.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <assert.h>
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
 * Private Data
 ****************************************************************************/

static uint32_t g_sequence;
static uint64_t g_utterance;
static unsigned int g_wake_label;
static unsigned int g_command_label;
static bool g_invoke_error;
static bool g_invoke_reset;
static bool g_invoke_overrun;

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
  assert(bytes == (wake ? VOICEFE_KWS_ROWS : VOICEFE_COMMAND_ROWS) * 40);
  if (g_invoke_error)
    {
      return -EIO;
    }

  if (!wake && g_invoke_reset)
    {
      atomic_store(&g_ml.reset, true);
    }

  if (!wake && g_invoke_overrun)
    {
      atomic_fetch_add(&g_ml.discontinuity, 1);
    }

  score->label = wake ? g_wake_label : g_command_label;
  score->score = 30000;
  score->margin = 20000;
  *cycles = 1;
  *elapsed_us = 1;
  return 0;
}

size_t tflm_titan_intent_arena(bool wake)
{
  return 0;
}

const char *tflm_titan_intent_version(void)
{
  return "scripted-host-test";
}

static void block(uint64_t id, uint64_t first, bool speech, bool end)
{
  struct titan_intent_block_s value;

  memset(&value, 0, sizeof(value));
  value.sequence = g_sequence++;
  value.generation = g_ml.gate.state.generation;
  value.utterance_id = id;
  value.start_sample = first;
  value.speech = speech;
  value.marker_end = end;
  voicefe_ml_block(&value);
}

static void turn(unsigned int wake, unsigned int command,
                 unsigned int speech_blocks)
{
  uint64_t id = ++g_utterance;
  uint64_t first = (uint64_t)g_sequence * 160;
  unsigned int i;

  g_wake_label = wake;
  g_command_label = command;
  for (i = 0; i < speech_blocks; i++)
    {
      block(id, first, true, false);
    }

  for (i = 0; i < 80; i++)
    {
      block(id, first, false, i == 79);
    }
}

int main(void)
{
  struct titan_intent_policy_s policy =
  {
    8000, 1000, 20000, 3000, 20000, 3000
  };

  uint64_t seq;

  titan_intent_init(&g_ml.gate, &policy, 1);
  voicefe_ml_clear();
  for (unsigned int i = 0; i < 60; i++)
    {
      block(0, 0, false, false);
    }

  turn(TITAN_WAKE_UNKNOWN, TITAN_PHOTO_TAKE, 20);
  assert(g_ml.gate.state.seq == 0);
  turn(TITAN_WAKE_INLINE, TITAN_PHOTO_TAKE, 20);
  assert(g_ml.gate.state.seq == 1);
  turn(TITAN_WAKE_WINDOW, TITAN_VIDEO_START, 20);
  assert(g_ml.gate.state.seq == 2);
  assert(!g_ml.gate.state.active[TITAN_MODE_VIDEO]);
  assert(g_ml.gate.state.wake_deadline != 0);
  turn(TITAN_WAKE_UNKNOWN, TITAN_VIDEO_START, 20);
  assert(g_ml.gate.state.active[TITAN_MODE_VIDEO]);
  assert(g_ml.gate.state.wake_deadline == 0);

  turn(TITAN_WAKE_INLINE, TITAN_COMPANION_ENTER, 20);
  assert(g_ml.gate.state.active[TITAN_MODE_COMPANION]);
  turn(TITAN_WAKE_UNKNOWN, TITAN_COMPANION_EXIT, 20);
  assert(!g_ml.gate.state.active[TITAN_MODE_COMPANION]);

  seq = g_ml.gate.state.seq;
  turn(TITAN_WAKE_INLINE, TITAN_PHOTO_TAKE, 401);
  assert(g_ml.gate.state.seq == seq);
  assert(g_ml.gate.state.too_long == 1);
  turn(TITAN_WAKE_INLINE, TITAN_PHOTO_TAKE, 390);
  assert(g_ml.gate.state.seq == seq + 1);

  seq = g_ml.gate.state.seq;
  g_invoke_error = true;
  turn(TITAN_WAKE_INLINE, TITAN_PHOTO_TAKE, 20);
  assert(g_ml.gate.state.seq == seq && g_ml.stats.failures != 0);
  g_invoke_error = false;
  g_invoke_overrun = true;
  turn(TITAN_WAKE_INLINE, TITAN_PHOTO_TAKE, 20);
  assert(g_ml.gate.state.seq == seq);
  g_invoke_overrun = false;
  g_invoke_reset = true;
  turn(TITAN_WAKE_INLINE, TITAN_PHOTO_TAKE, 20);
  assert(g_ml.gate.state.seq == seq);
  puts("PASS deployed ML stream: endpoints, wake window, companion,"
       " 4-second bound, NPU failure, in-flight overrun and reset"
       " rejection");
  return 0;
}
