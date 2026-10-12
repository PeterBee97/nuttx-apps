/****************************************************************************
 * apps/system/titan_voice/voicefe_intent.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <audioutils/titan_voicefe.h>

#include "voicefe_intent.h"
#include "../../mlearning/tflite-micro/models/titan_intent/metadata.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VI_QUEUE CONFIG_SYSTEM_TITAN_VOICE_INTENT_QUEUE
#define VI_ROWS VOICEFE_COMMAND_ROWS
#define VI_COLUMNS VOICEFE_MELS
#define VI_EVENTS 32
#define VI_PREROLL_ROWS 15
#define VI_END_BLOCKS (CONFIG_SYSTEM_TITAN_VOICE_INTENT_END_MS / 10)
#define VI_RING_ROWS (VI_ROWS + VI_PREROLL_ROWS + VI_END_BLOCKS / 2 + 1)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct voicefe_ml_s
{
  pthread_t thread;
  sem_t wake;
  atomic_bool stop;
  atomic_bool reset;
  atomic_bool upload;
  atomic_uint generation;
  atomic_uint discontinuity;
  atomic_uint head;
  atomic_uint tail;
  atomic_uint overruns;
  atomic_uint highwater;
  struct titan_intent_block_s queue[VI_QUEUE];
  struct titan_intent_s gate;
  struct voicefe_features_s features;
  struct voicefe_intent_stats_s stats;
  int8_t ring[VI_RING_ROWS][VI_COLUMNS];
  int8_t snapshot[2][VI_ROWS * VI_COLUMNS];
  struct titan_intent_event_s events[VI_EVENTS];
  struct titan_intent_event_s recent[8];
  unsigned int event_head;
  unsigned int event_tail;
  unsigned int recent_head;
  uint64_t epoch;
  uint64_t skip_turn;
  uint64_t turn_row;
  uint64_t rows;
  uint64_t last_speech_sample;
  uint32_t expected;
  uint32_t observed_discontinuity;
  unsigned int quiet;
  unsigned int wake_blocks;
  bool sequence_valid;
  bool evaluated;
  bool snapshot_busy[2];
  bool running;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct voicefe_ml_s g_ml;
static pthread_mutex_t g_ml_lock = PTHREAD_MUTEX_INITIALIZER;
static struct voicefe_intent_stats_s g_snapshot;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint64_t voicefe_ml_now(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void voicefe_ml_publish(void)
{
  atomic_store(&g_ml.upload,
               g_ml.gate.state.active[TITAN_MODE_COMPANION]);
  atomic_store(&g_ml.generation, g_ml.gate.state.generation);
  g_ml.stats.state = g_ml.gate.state;
  g_ml.stats.overruns = atomic_load(&g_ml.overruns);
  g_ml.stats.highwater = atomic_load(&g_ml.highwater);
  pthread_mutex_lock(&g_ml_lock);
  g_snapshot = g_ml.stats;
  pthread_mutex_unlock(&g_ml_lock);
}

static void voicefe_ml_event(const struct titan_intent_event_s *event)
{
  voicefe_ml_publish();
  pthread_mutex_lock(&g_ml_lock);
  g_ml.recent[g_ml.recent_head++ % 8] = *event;
  if (g_ml.event_head - g_ml.event_tail == VI_EVENTS)
    {
      g_ml.stats.event_drop++;
    }
  else
    {
      g_ml.events[g_ml.event_head++ % VI_EVENTS] = *event;
    }

  pthread_mutex_unlock(&g_ml_lock);
}

static void voicefe_ml_clear(void)
{
  titan_intent_cancel(&g_ml.gate);
  voicefe_features_init(&g_ml.features);
  memset(g_ml.ring, VOICEFE_FEATURE_SILENCE, sizeof(g_ml.ring));
  g_ml.rows = 0;
  g_ml.quiet = 0;
  g_ml.wake_blocks = 0;
  g_ml.evaluated = false;
}

static bool voicefe_ml_copy(unsigned int slot, uint64_t first,
                            unsigned int count)
{
  unsigned int rows = slot == 0 ? VOICEFE_KWS_ROWS : VI_ROWS;
  unsigned int i;

  if (g_ml.snapshot_busy[slot] || count > rows ||
      first + count > g_ml.rows ||
      (g_ml.rows > VI_RING_ROWS && first < g_ml.rows - VI_RING_ROWS))
    {
      g_ml.stats.snapshot_busy++;
      return false;
    }

  g_ml.snapshot_busy[slot] = true;
  memset(g_ml.snapshot[slot], VOICEFE_FEATURE_SILENCE,
         rows * VI_COLUMNS);
  for (i = 0; i < count; i++)
    {
      memcpy(g_ml.snapshot[slot] + i * VI_COLUMNS,
             g_ml.ring[(first + i) % VI_RING_ROWS], VI_COLUMNS);
    }

  return true;
}

static int voicefe_ml_invoke(unsigned int slot,
                             struct titan_intent_score_s *score)
{
  uint64_t cycles = 0;
  uint32_t elapsed = 0;
  bool wake = slot == 0;
  uint32_t discontinuity = atomic_load(&g_ml.discontinuity);
  int ret;

  ret = tflm_titan_intent_invoke(wake, g_ml.snapshot[slot],
                                (wake ? VOICEFE_KWS_ROWS : VI_ROWS) *
                                VI_COLUMNS, score, &cycles, &elapsed);
  g_ml.snapshot_busy[slot] = false;
  if (ret == 0 && (atomic_load(&g_ml.reset) ||
                  atomic_load(&g_ml.stop) ||
                  discontinuity != atomic_load(&g_ml.discontinuity)))
    {
      g_ml.skip_turn = g_ml.gate.current;
      voicefe_ml_clear();
      return -ECANCELED;
    }

  if (ret < 0)
    {
      g_ml.stats.failures++;
      g_ml.skip_turn = g_ml.gate.current;
      voicefe_ml_clear();
      return ret;
    }

  if (wake)
    {
      g_ml.stats.wake_invokes++;
      g_ml.stats.wake_cycles = cycles;
      g_ml.stats.wake_us = elapsed;
    }
  else
    {
      g_ml.stats.command_invokes++;
      g_ml.stats.command_cycles = cycles;
      g_ml.stats.command_us = elapsed;
      g_ml.stats.last_command = *score;
    }

  return 0;
}

static void voicefe_ml_block(const struct titan_intent_block_s *block)
{
  struct titan_intent_score_s score;
  struct titan_intent_event_s event;
  int8_t row[VI_COLUMNS];
  uint64_t now = g_ml.epoch + (uint64_t)block->sequence * 10;
  uint64_t duration;
  unsigned int count;
  uint64_t first;

  if (block->generation != g_ml.gate.state.generation)
    {
      g_ml.skip_turn = block->utterance_id;
      return;
    }

  if ((g_ml.sequence_valid && block->sequence != g_ml.expected) ||
      g_ml.observed_discontinuity != atomic_load(&g_ml.discontinuity))
    {
      g_ml.observed_discontinuity = atomic_load(&g_ml.discontinuity);
      g_ml.skip_turn = block->utterance_id;
      voicefe_ml_clear();
    }

  g_ml.sequence_valid = true;
  g_ml.expected = block->sequence + 1;
  if (voicefe_features_push(&g_ml.features, block->pcm, row))
    {
      memcpy(g_ml.ring[g_ml.rows++ % VI_RING_ROWS], row, sizeof(row));
    }

  /* The first 500 ms are the existing VAD background calibration. */

  if (block->sequence < 50)
    {
      return;
    }

  if (block->utterance_id != 0 &&
      block->utterance_id != g_ml.skip_turn &&
      !g_ml.gate.in_turn &&
      titan_intent_begin(&g_ml.gate, block->utterance_id,
                         g_ml.epoch + block->start_sample / 16,
                         block->generation))
    {
      g_ml.turn_row = g_ml.rows > VI_PREROLL_ROWS ?
                      g_ml.rows - VI_PREROLL_ROWS : 0;
      g_ml.quiet = 0;
      g_ml.evaluated = false;
      g_ml.last_speech_sample = block->start_sample;
    }

  if (g_ml.gate.in_turn && block->utterance_id == g_ml.gate.current)
    {
      if (block->speech)
        {
          g_ml.last_speech_sample =
            (uint64_t)(block->sequence + 1) * VOICEFE_FRAMES;
          g_ml.quiet = 0;
          if (!g_ml.gate.emitted)
            {
              g_ml.evaluated = false;
            }
        }
      else
        {
          g_ml.quiet++;
        }

      duration = g_ml.last_speech_sample - block->start_sample;
      if (!g_ml.evaluated && g_ml.quiet >= VI_END_BLOCKS)
        {
          memset(&score, 0, sizeof(score));
          score.label = TITAN_UNKNOWN;
          g_ml.evaluated = true;
          first = g_ml.turn_row;
          count = g_ml.rows - first;
          if (count > VI_ROWS)
            {
              /* Remove only endpoint silence and then excess pre-roll.
               * Preserve the speech onset of a command near four seconds.
               */

              count -= g_ml.quiet / 2;
              if (count > VI_ROWS)
                {
                  first += count - VI_ROWS;
                  count = VI_ROWS;
                }
            }

          if (duration > CONFIG_SYSTEM_TITAN_VOICE_INTENT_MAX_MS * 16u)
            {
              titan_intent_candidate(&g_ml.gate, block->utterance_id,
                                      block->generation, now, &score, true,
                                      &event);
            }
          else if (voicefe_ml_copy(1, first, count) &&
                   voicefe_ml_invoke(1, &score) == 0 &&
                   !atomic_load(&g_ml.reset) &&
                   titan_intent_candidate(&g_ml.gate, block->utterance_id,
                                           block->generation, now, &score,
                                           false, &event))
            {
              voicefe_ml_event(&event);
            }
        }
    }

  /* Command inference has priority over periodic wake inference. */

  if (++g_ml.wake_blocks >= 10)
    {
      g_ml.wake_blocks = 0;
      first = g_ml.rows > VOICEFE_KWS_ROWS ?
              g_ml.rows - VOICEFE_KWS_ROWS : 0;
      count = g_ml.rows - first;
      if (voicefe_ml_copy(0, first, count) &&
          voicefe_ml_invoke(0, &score) == 0)
        {
          titan_intent_wake(&g_ml.gate, &score);
        }
    }

  if (block->marker_end &&
      titan_intent_end(&g_ml.gate, block->utterance_id, now, &event))
    {
      voicefe_ml_event(&event);
    }
}

static void *voicefe_ml_worker(void *arg)
{
  struct titan_intent_event_s event;
  unsigned int head;
  unsigned int tail;

  while (!atomic_load(&g_ml.stop))
    {
      while (sem_wait(&g_ml.wake) < 0 && errno == EINTR)
        {
        }

      if (atomic_load(&g_ml.stop))
        {
          break;
        }

      if (atomic_exchange(&g_ml.reset, false))
        {
          g_ml.skip_turn = g_ml.gate.current;
          titan_intent_reset(&g_ml.gate, voicefe_ml_now(), &event);
          voicefe_ml_clear();
          voicefe_ml_event(&event);
          voicefe_ml_publish();
        }

      tail = atomic_load(&g_ml.tail);
      head = atomic_load(&g_ml.head);
      if (tail == head)
        {
          continue;
        }

      voicefe_ml_block(&g_ml.queue[tail % VI_QUEUE]);
      atomic_store(&g_ml.tail, tail + 1);
      voicefe_ml_publish();
    }

  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int voicefe_intent_start(void)
{
  struct titan_intent_policy_s policy =
  {
    CONFIG_SYSTEM_TITAN_VOICE_INTENT_WINDOW_MS,
    CONFIG_SYSTEM_TITAN_VOICE_INTENT_COOLDOWN_MS,
    CONFIG_SYSTEM_TITAN_VOICE_INTENT_WAKE_SCORE < 0 ?
    TITAN_MODEL_WAKE_THRESHOLD : CONFIG_SYSTEM_TITAN_VOICE_INTENT_WAKE_SCORE,
    CONFIG_SYSTEM_TITAN_VOICE_INTENT_WAKE_MARGIN < 0 ?
    TITAN_MODEL_WAKE_MARGIN : CONFIG_SYSTEM_TITAN_VOICE_INTENT_WAKE_MARGIN,
    CONFIG_SYSTEM_TITAN_VOICE_INTENT_COMMAND_SCORE < 0 ?
    TITAN_MODEL_COMMAND_THRESHOLD :
    CONFIG_SYSTEM_TITAN_VOICE_INTENT_COMMAND_SCORE,
    CONFIG_SYSTEM_TITAN_VOICE_INTENT_COMMAND_MARGIN < 0 ?
    TITAN_MODEL_COMMAND_MARGIN :
    CONFIG_SYSTEM_TITAN_VOICE_INTENT_COMMAND_MARGIN
  };

  struct sched_param param;
  pthread_attr_t attr;
  int ret;

  if (g_ml.running)
    {
      return -EBUSY;
    }

  memset(&g_ml, 0, sizeof(g_ml));
  memset(&g_snapshot, 0, sizeof(g_snapshot));
  ret = tflm_titan_intent_init();
  if (ret < 0)
    {
      return ret;
    }

  titan_intent_init(&g_ml.gate, &policy, 1);
  voicefe_ml_clear();
  atomic_store(&g_ml.generation, 1);
  if (sem_init(&g_ml.wake, 0, 0) < 0)
    {
      ret = -errno;
      tflm_titan_intent_deinit();
      return ret;
    }

  sem_setprotocol(&g_ml.wake, SEM_PRIO_NONE);
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr,
                           CONFIG_SYSTEM_TITAN_VOICE_INTENT_STACKSIZE);
  pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  param.sched_priority = CONFIG_SYSTEM_TITAN_VOICE_INTENT_PRIORITY;
  pthread_attr_setschedparam(&attr, &param);
  g_ml.epoch = voicefe_ml_now();
  ret = pthread_create(&g_ml.thread, &attr, voicefe_ml_worker, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      sem_destroy(&g_ml.wake);
      tflm_titan_intent_deinit();
      return -ret;
    }

  pthread_setname_np(g_ml.thread, "voicefe_ml");
  g_ml.running = true;
  printf("intent: model=%s%s arena=%zu/%zu bytes\n",
         tflm_titan_intent_version(), TITAN_MODEL_SMOKE ? " (smoke)" : "",
         tflm_titan_intent_arena(true), tflm_titan_intent_arena(false));
  return 0;
}

void voicefe_intent_stop(void)
{
  if (g_ml.running)
    {
      atomic_store(&g_ml.stop, true);
      sem_post(&g_ml.wake);
      pthread_join(g_ml.thread, NULL);
      sem_destroy(&g_ml.wake);
      tflm_titan_intent_deinit();
      g_ml.running = false;
    }
}

void voicefe_intent_reset(void)
{
  atomic_store(&g_ml.reset, true);
  sem_post(&g_ml.wake);
}

bool voicefe_intent_upload(void)
{
  return atomic_load(&g_ml.upload);
}

uint32_t voicefe_intent_generation(void)
{
  return atomic_load(&g_ml.generation);
}

void voicefe_intent_put(const int16_t *pcm, uint32_t sequence,
                       uint64_t utterance, uint64_t start_sample,
                       bool speech, bool marker_end)
{
  unsigned int head = atomic_load(&g_ml.head);
  unsigned int count = head - atomic_load(&g_ml.tail);
  struct titan_intent_block_s *block;

  if (count == VI_QUEUE)
    {
      atomic_fetch_add(&g_ml.overruns, 1);
      atomic_fetch_add(&g_ml.discontinuity, 1);
      return;
    }

  block = &g_ml.queue[head % VI_QUEUE];
  memcpy(block->pcm, pcm, sizeof(block->pcm));
  block->sequence = sequence;
  block->utterance_id = utterance;
  block->start_sample = start_sample;
  block->generation = atomic_load(&g_ml.generation);
  block->speech = speech;
  block->marker_end = marker_end;
  atomic_store(&g_ml.head, head + 1);
  if (count + 1 > atomic_load(&g_ml.highwater))
    {
      atomic_store(&g_ml.highwater, count + 1);
    }

  sem_post(&g_ml.wake);
}

void voicefe_intent_stats(struct voicefe_intent_stats_s *stats)
{
  pthread_mutex_lock(&g_ml_lock);
  *stats = g_snapshot;
  pthread_mutex_unlock(&g_ml_lock);
}

bool voicefe_intent_poll(struct titan_intent_event_s *event)
{
  bool available;

  pthread_mutex_lock(&g_ml_lock);
  available = g_ml.event_tail != g_ml.event_head;
  if (available)
    {
      *event = g_ml.events[g_ml.event_tail++ % VI_EVENTS];
    }

  pthread_mutex_unlock(&g_ml_lock);
  return available;
}

void voicefe_intent_print(bool events)
{
  struct voicefe_intent_stats_s stats;
  struct titan_intent_event_s event;
  unsigned int first;
  unsigned int i;

  voicefe_intent_stats(&stats);
  printf("intent: video(simulated)=%d audio(simulated)=%d companion=%d"
         " window=%" PRIu64 " generation=%" PRIu32 "\n",
         stats.state.active[0], stats.state.active[1], stats.state.active[2],
         stats.state.wake_deadline, stats.state.generation);
  printf("ML queue=%" PRIu32 "/%d overrun=%" PRIu32
         " snapshot_busy=%" PRIu32 " event_drop=%" PRIu32 "\n",
         stats.highwater, VI_QUEUE, stats.overruns, stats.snapshot_busy,
         stats.event_drop);
  printf("U55 wake=%" PRIu32 " (%" PRIu64 " cycles, %" PRIu32
         " us) command=%" PRIu32 " (%" PRIu64 " cycles, %" PRIu32
         " us) failed=%" PRIu32 "\n", stats.wake_invokes, stats.wake_cycles,
         stats.wake_us, stats.command_invokes, stats.command_cycles,
         stats.command_us, stats.failures);
  printf("command=%s score=%u margin=%u unknown=%" PRIu32
         " denied=%" PRIu32 " too_long=%" PRIu32 "\n",
         titan_intent_name(stats.last_command.label),
         stats.last_command.score,
         stats.last_command.margin, stats.state.unknown, stats.state.denied,
         stats.state.too_long);
  if (!events)
    {
      return;
    }

  pthread_mutex_lock(&g_ml_lock);
  first = g_ml.recent_head > 8 ? g_ml.recent_head - 8 : 0;
  for (i = first; i < g_ml.recent_head; i++)
    {
      event = g_ml.recent[i % 8];
      printf("seq=%" PRIu64 " utterance=%" PRIu64 " %s %s %s target=%d\n",
             event.seq, event.utterance_id,
             titan_intent_event_name(event.kind),
             titan_intent_name(event.intent),
             titan_intent_result_name(event.result), event.resolved);
    }

  pthread_mutex_unlock(&g_ml_lock);
}
