/****************************************************************************
 * apps/testing/titan_asr/asr_frontend.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/clock.h>
#ifdef CONFIG_TESTING_TITAN_ASR_SNAPSHOT_TRACE
#  include <nuttx/irq.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef CONFIG_TESTING_TITAN_ASR_ASYNC_DUMP
#  include <pthread.h>
#  include <sched.h>
#  include <semaphore.h>
#endif
#include <audioutils/titan_voicefe.h>

#include "asr_frontend.h"
#include "asr_resample.h"
#include "speex/speex/speex_echo.h"
#ifdef CONFIG_TESTING_TITAN_ASR_RESIDUAL
#  include "speex/speex/speex_preprocess.h"
#endif
#include "speex/config.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifdef CONFIG_TESTING_TITAN_ASR_RESIDUAL
#  define AEC_ARENA_BYTES 98304
#else
#  define AEC_ARENA_BYTES 65536
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct asr_frontend_s
{
  struct asr_resample_s resample[3];
  struct voicefe_dsp_s beam;
  struct voicefe_ns_s ns;
  void *echo;
  void *preprocess;
  uint8_t *arena;
  size_t used;
  unsigned int mode;
  unsigned int count;
  unsigned int blocks;
  uint64_t ticks;
  uint32_t maximum;
#ifdef CONFIG_TESTING_TITAN_ASR_SNAPSHOT_TRACE
  uint64_t stages[3];
#endif
  uint64_t energy[6];
#ifdef CONFIG_TESTING_TITAN_ASR_ADAPTIVE_SUPPRESS
  uint32_t speech_power;
  unsigned int speech_confirm;
  unsigned int speech_hold;
  unsigned int relaxed_blocks;
  int active_suppression;
#endif
  int dump;
#ifdef CONFIG_TESTING_TITAN_ASR_BUFFERED_DUMP
  int16_t (*recording)[160][6];
#endif
#ifdef CONFIG_TESTING_TITAN_ASR_ASYNC_DUMP
  int16_t (*dump_queue)[160][6];
  pthread_t dump_thread;
  pthread_mutex_t dump_lock;
  sem_t dump_ready;
  unsigned int dump_read;
  unsigned int dump_write;
  unsigned int dump_count;
  unsigned int dump_peak;
  int dump_error;
  bool dump_done;
#endif
  int16_t mic[320];
  int16_t reference[160];
  int16_t scaled_reference[160];
  int16_t cancelled[320];
  int16_t history[160][2];
  unsigned int history_index;
  int16_t record[160][6];
  jmp_buf failure;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct asr_frontend_s *g_frontend;

#ifdef CONFIG_TESTING_TITAN_ASR_ASYNC_DUMP
static void *asr_dump_worker(void *argument)
{
  struct asr_frontend_s *state = argument;

  for (; ; )
    {
      size_t offset = 0;
      unsigned int slot;

      while (sem_wait(&state->dump_ready) < 0 && errno == EINTR)
        {
        }

      pthread_mutex_lock(&state->dump_lock);
      if (state->dump_count == 0 && state->dump_done)
        {
          pthread_mutex_unlock(&state->dump_lock);
          break;
        }

      slot = state->dump_read % 128;
      pthread_mutex_unlock(&state->dump_lock);
      while (offset < sizeof(state->record))
        {
          ssize_t size = write(state->dump,
                              (uint8_t *)state->dump_queue[slot] + offset,
                              sizeof(state->record) - offset);

          if (size <= 0)
            {
              int error = size < 0 ? -errno : -EIO;

              pthread_mutex_lock(&state->dump_lock);
              state->dump_error = error;
              pthread_mutex_unlock(&state->dump_lock);
              return NULL;
            }

          offset += size;
        }

      pthread_mutex_lock(&state->dump_lock);
      state->dump_read++;
      state->dump_count--;
      pthread_mutex_unlock(&state->dump_lock);
    }

  return NULL;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#ifdef CONFIG_TESTING_TITAN_ASR_SNAPSHOT_TRACE
uint64_t asr_frontend_ticks(void)
{
  irqstate_t flags = enter_critical_section();
  uint64_t ticks = g_frontend == NULL ? 0 : g_frontend->ticks;

  leave_critical_section(flags);
  return ticks;
}

int asr_frontend_diagnostic(int suppression, int noise)
{
#ifdef CONFIG_TESTING_TITAN_ASR_RESIDUAL
  struct asr_frontend_s *state = g_frontend;

  if (state == NULL || state->preprocess == NULL)
    {
      return -EINVAL;
    }

  if (suppression == 0)
    {
      speex_preprocess_ctl(state->preprocess,
                           SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, &suppression);
      speex_preprocess_ctl(state->preprocess,
                           SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE,
                           &suppression);
    }

  if (noise == 0)
    {
      speex_preprocess_ctl(state->preprocess,
                           SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &noise);
    }

  return 0;
#else
  return -ENOSYS;
#endif
}
#endif

/* The selected Speex cancellation/FFT paths allocate only during init.
 * A bounded arena makes partial initialization failure recoverable despite
 * upstream initializers not checking every individual allocation.
 */

void *asr_aec_allocate(int size)
{
  struct asr_frontend_s *state = g_frontend;
  size_t aligned = ((size_t)size + 7) & ~(size_t)7;
  void *result;

  if (size < 0 || aligned > AEC_ARENA_BYTES - state->used)
    {
      longjmp(state->failure, 1);
    }

  result = state->arena + state->used;
  state->used += aligned;
  return result;
}

int asr_frontend_open(unsigned int mode, int dump)
{
  struct asr_frontend_s *state;
  int rate = 16000;

  if (g_frontend != NULL)
    {
      return -EBUSY;
    }

  if (mode >
#ifdef CONFIG_TESTING_TITAN_ASR_RESIDUAL
      4
#else
      3
#endif
     )
    {
      return -EINVAL;
    }

  state = calloc(1, sizeof(*state));
  if (state == NULL)
    {
      return -ENOMEM;
    }

  g_frontend = state;
  state->dump = -1;
  state->mode = mode;
  if (mode != 0)
    {
      state->arena = calloc(1, AEC_ARENA_BYTES);
      if (state->arena == NULL || setjmp(state->failure) != 0)
        {
          asr_frontend_close();
          return -ENOMEM;
        }

      state->echo = speex_echo_state_init_mc(
        160, CONFIG_TESTING_TITAN_ASR_AEC_TAIL, 2, 1);
      speex_echo_ctl(state->echo, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
#ifdef CONFIG_TESTING_TITAN_ASR_RESIDUAL
      if (mode == 4)
        {
          int suppression = CONFIG_TESTING_TITAN_ASR_ECHO_SUPPRESS;
          int active = CONFIG_TESTING_TITAN_ASR_ECHO_SUPPRESS_ACTIVE;
          int noise = -15;

#ifdef CONFIG_TESTING_TITAN_ASR_ADAPTIVE_SUPPRESS
          state->active_suppression = active;
#endif
          state->preprocess = speex_preprocess_state_init(160, rate);
          speex_preprocess_ctl(state->preprocess,
                               SPEEX_PREPROCESS_SET_ECHO_STATE, state->echo);
          speex_preprocess_ctl(state->preprocess,
                               SPEEX_PREPROCESS_SET_ECHO_SUPPRESS,
                               &suppression);
          speex_preprocess_ctl(state->preprocess,
                               SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE,
                               &active);
          speex_preprocess_ctl(state->preprocess,
                               SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &noise);
        }
#endif
    }

  voicefe_dsp_init(&state->beam, mode >= 2 ? VOICEFE_BEAM : VOICEFE_MEAN,
                  32768, 32768);
  voicefe_ns_init(&state->ns, CONFIG_AUDIOUTILS_TITAN_VOICEFE_NS_FLOOR);
  if (dump)
    {
      state->dump = open("/dev/ttyACM0", O_WRONLY);
      if (state->dump < 0)
        {
          int error = -errno;

          asr_frontend_close();
          return error;
        }

#ifdef CONFIG_TESTING_TITAN_ASR_BUFFERED_DUMP
      state->recording = malloc(6000 * sizeof(*state->recording));
      if (state->recording == NULL)
        {
          asr_frontend_close();
          return -ENOMEM;
        }
#endif

#ifdef CONFIG_TESTING_TITAN_ASR_ASYNC_DUMP
      pthread_attr_t attr;
      struct sched_param priority =
        {
          .sched_priority = CONFIG_TESTING_TITAN_ASR_CAPTURE_PRIORITY > 1 ?
                            CONFIG_TESTING_TITAN_ASR_CAPTURE_PRIORITY - 1 : 1
        };

      int ret;

      state->dump_queue = calloc(128, sizeof(*state->dump_queue));
      if (state->dump_queue == NULL)
        {
          asr_frontend_close();
          return -ENOMEM;
        }

      pthread_mutex_init(&state->dump_lock, NULL);
      sem_init(&state->dump_ready, 0, 0);
      pthread_attr_init(&attr);
      pthread_attr_setstacksize(&attr, 2048);
      pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
      pthread_attr_setschedparam(&attr, &priority);
      ret = pthread_create(&state->dump_thread, &attr,
                           asr_dump_worker, state);
      pthread_attr_destroy(&attr);
      if (ret != 0)
        {
          sem_destroy(&state->dump_ready);
          pthread_mutex_destroy(&state->dump_lock);
          free(state->dump_queue);
          state->dump_queue = NULL;
          asr_frontend_close();
          return -ret;
        }
#endif
    }

  printf("ASR_FRONTEND mode=%u arena=%zu reserved=%u state=%zu "
         "tail=%u rate=%u dump=%d\n",
         mode, state->used, mode ? AEC_ARENA_BYTES : 0,
         sizeof(*state), CONFIG_TESTING_TITAN_ASR_AEC_TAIL,
         CONFIG_TESTING_TITAN_ASR_MIC_RATE, dump);
  return 0;
}

static int asr_frontend_process(int16_t *mono)
{
  struct asr_frontend_s *state = g_frontend;
  uint32_t start = perf_gettime();
  uint32_t elapsed;
  unsigned int channel;
#ifdef CONFIG_TESTING_TITAN_ASR_SNAPSHOT_TRACE
  uint32_t profile[4];

  profile[0] = start;
#endif

  if (state->echo != NULL)
    {
      for (channel = 0; channel < 160; channel++)
        {
          int32_t sample = (int32_t)state->reference[channel] *
                           CONFIG_TESTING_TITAN_ASR_REFERENCE_GAIN;

          state->scaled_reference[channel] =
            sample > INT16_MAX ? INT16_MAX :
            sample < INT16_MIN ? INT16_MIN : sample;
        }

      speex_echo_cancellation(state->echo, state->mic,
                              state->scaled_reference, state->cancelled);
#ifdef CONFIG_TESTING_TITAN_ASR_SNAPSHOT_TRACE
      profile[1] = perf_gettime();
#endif
      voicefe_process(&state->beam, state->cancelled, mono);
    }
  else
    {
      memcpy(state->cancelled, state->mic, sizeof(state->mic));
#ifdef CONFIG_TESTING_TITAN_ASR_SNAPSHOT_TRACE
      profile[1] = perf_gettime();
#endif
      for (channel = 0; channel < 160; channel++)
        {
          mono[channel] = ((int32_t)state->mic[2 * channel] +
                           state->mic[2 * channel + 1]) / 2;
        }
    }

#ifdef CONFIG_TESTING_TITAN_ASR_SNAPSHOT_TRACE
  profile[2] = perf_gettime();
#endif
  if (state->mode == 3)
    {
      voicefe_ns_process(&state->ns, mono);
    }

#ifdef CONFIG_TESTING_TITAN_ASR_RESIDUAL
  if (state->preprocess != NULL)
    {
#ifdef CONFIG_TESTING_TITAN_ASR_ADAPTIVE_SUPPRESS
      uint64_t power = 0;
      int active;

      for (channel = 0; channel < 160; channel++)
        {
          power += (int32_t)mono[channel] * mono[channel];
        }

      state->speech_power = (7ull * state->speech_power + power / 160) / 8;
      if (state->speech_power >=
          CONFIG_TESTING_TITAN_ASR_ADAPTIVE_POWER)
        {
          if (state->speech_confirm <
              CONFIG_TESTING_TITAN_ASR_ADAPTIVE_CONFIRM)
            {
              state->speech_confirm++;
            }

          if (state->speech_confirm >=
              CONFIG_TESTING_TITAN_ASR_ADAPTIVE_CONFIRM)
            {
              state->speech_hold = CONFIG_TESTING_TITAN_ASR_ADAPTIVE_HOLD;
            }
        }
      else
        {
          state->speech_confirm = 0;
          if (state->speech_hold > 0)
            {
              state->speech_hold--;
            }
        }

      active = state->speech_hold > 0 ?
               CONFIG_TESTING_TITAN_ASR_ADAPTIVE_DB :
               CONFIG_TESTING_TITAN_ASR_ECHO_SUPPRESS_ACTIVE;
      if (active != state->active_suppression)
        {
          speex_preprocess_ctl(state->preprocess,
                               SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE,
                               &active);
          state->active_suppression = active;
#ifdef CONFIG_TESTING_TITAN_ASR_ADAPTIVE_TRACE
          printf("ASR_SUPPRESS block=%u power=%" PRIu32 " active=%d\n",
                 state->blocks, state->speech_power, active);
#endif
        }

      state->relaxed_blocks += state->speech_hold > 0;
#endif
      speex_preprocess_run(state->preprocess, mono);
    }
#endif

#ifdef CONFIG_TESTING_TITAN_ASR_SNAPSHOT_TRACE
  profile[3] = perf_gettime();
  for (channel = 0; channel < 3; channel++)
    {
      state->stages[channel] +=
        (uint32_t)(profile[channel + 1] - profile[channel]);
    }

#endif
  elapsed = (uint32_t)perf_gettime() - start;
  state->ticks += elapsed;
  if (elapsed > state->maximum)
    {
      state->maximum = elapsed;
    }

  for (unsigned int frame = 0; frame < 160; frame++)
    {
      int16_t *record = state->record[frame];

      record[0] = state->mic[2 * frame];
      record[1] = state->mic[2 * frame + 1];
      record[2] = state->reference[frame];
      record[3] = state->cancelled[2 * frame];
      record[4] = state->cancelled[2 * frame + 1];
      record[5] = mono[frame];
      for (channel = 0; channel < 6; channel++)
        {
          state->energy[channel] +=
            (int32_t)record[channel] * record[channel];
        }
    }

  if (state->dump >= 0)
    {
#ifdef CONFIG_TESTING_TITAN_ASR_BUFFERED_DUMP
      if (state->blocks >= 6000)
        {
          return -ENOSPC;
        }

      memcpy(state->recording[state->blocks],
             state->record, sizeof(state->record));
#elif defined(CONFIG_TESTING_TITAN_ASR_ASYNC_DUMP)
      int error;

      pthread_mutex_lock(&state->dump_lock);
      error = state->dump_error;
      if (error == 0 && state->dump_count == 128)
        {
          error = -ENOSPC;
        }

      if (error == 0)
        {
          memcpy(state->dump_queue[state->dump_write++ % 128],
                 state->record, sizeof(state->record));
          state->dump_count++;
          if (state->dump_peak < state->dump_count)
            {
              state->dump_peak = state->dump_count;
            }
        }

      pthread_mutex_unlock(&state->dump_lock);
      if (error != 0)
        {
          return error;
        }

      sem_post(&state->dump_ready);
#else
      size_t offset = 0;

      while (offset < sizeof(state->record))
        {
          ssize_t size = write(state->dump,
                              (uint8_t *)state->record + offset,
                              sizeof(state->record) - offset);

          if (size <= 0)
            {
              return size < 0 ? -errno : -EIO;
            }

          offset += size;
        }
#endif
    }

  state->blocks++;
  return 160;
}

int asr_frontend_replay(const int16_t *input, int16_t *mono)
{
  struct asr_frontend_s *state = g_frontend;
  unsigned int frame;

  for (frame = 0; frame < 160; frame++)
    {
      state->mic[2 * frame] = input[3 * frame];
      state->mic[2 * frame + 1] = input[3 * frame + 1];
      state->reference[frame] = input[3 * frame + 2];
    }

  return asr_frontend_process(mono);
}

int asr_frontend_push(const int16_t *input, int16_t *mono)
{
  struct asr_frontend_s *state = g_frontend;
  unsigned int i;
  unsigned int channel;
  int16_t sample[3];
  bool ready;
  int emitted = 0;

  for (i = 0; i < 160; i++)
    {
      ready = false;
      for (channel = 0; channel < 3; channel++)
        {
          ready = asr_resample(&state->resample[channel],
                              input[3 * i + channel],
                              CONFIG_TESTING_TITAN_ASR_MIC_RATE,
                              &sample[channel]);
        }

      if (!ready)
        {
          continue;
        }

      state->mic[2 * state->count] =
        state->history[state->history_index][0];
      state->mic[2 * state->count + 1] =
        state->history[state->history_index][1];
      state->history[state->history_index][0] = sample[0];
      state->history[state->history_index][1] = sample[1];
      state->history_index = (state->history_index + 1) % 160;
      state->reference[state->count] = sample[2];
      if (++state->count != 160)
        {
          continue;
        }

      emitted = asr_frontend_process(mono);
      state->count = 0;
      if (emitted < 0)
        {
          return emitted;
        }
    }

  return emitted;
}

void asr_frontend_close(void)
{
  struct asr_frontend_s *state = g_frontend;

  if (state == NULL)
    {
      return;
    }

#ifdef CONFIG_TESTING_TITAN_ASR_BUFFERED_DUMP
  if (state->recording != NULL)
    {
      size_t bytes = state->blocks * sizeof(*state->recording);
      size_t offset = 0;
      int error = 0;

      while (offset < bytes)
        {
          ssize_t size = write(state->dump,
                              (uint8_t *)state->recording + offset,
                              bytes - offset);

          if (size <= 0)
            {
              error = size < 0 ? -errno : -EIO;
              break;
            }

          offset += size;
        }

      printf("ASR_BUFFERED_DUMP bytes=%zu expected=%zu error=%d\n",
             offset, bytes, error);
      free(state->recording);
    }
#endif

#ifdef CONFIG_TESTING_TITAN_ASR_ASYNC_DUMP
  if (state->dump_queue != NULL)
    {
      pthread_mutex_lock(&state->dump_lock);
      state->dump_done = true;
      pthread_mutex_unlock(&state->dump_lock);
      sem_post(&state->dump_ready);
      pthread_join(state->dump_thread, NULL);
      printf("ASR_DUMP blocks=%u peak=%u error=%d\n",
             state->dump_read, state->dump_peak, state->dump_error);
      sem_destroy(&state->dump_ready);
      pthread_mutex_destroy(&state->dump_lock);
      free(state->dump_queue);
    }
#endif

  printf("ASR_FRONTEND_DONE blocks=%u total_us=%" PRIu64
         " max_us=%" PRIu64 " energy=",
         state->blocks, state->ticks * 1000000 / perf_getfreq(),
         (uint64_t)state->maximum * 1000000 / perf_getfreq());
  for (unsigned int channel = 0; channel < 6; channel++)
    {
      printf("%" PRIu64 ",", state->energy[channel]);
    }

  printf("\n");
#ifdef CONFIG_TESTING_TITAN_ASR_SNAPSHOT_TRACE
  printf("ASR_FRONTEND_PROFILE aec=%" PRIu64 " beam=%" PRIu64
         " spectral=%" PRIu64 " us\n",
         state->stages[0] * 1000000 / perf_getfreq(),
         state->stages[1] * 1000000 / perf_getfreq(),
         state->stages[2] * 1000000 / perf_getfreq());
#endif
#ifdef CONFIG_TESTING_TITAN_ASR_ADAPTIVE_SUPPRESS
  printf("ASR_SUPPRESSION relaxed_blocks=%u threshold=%u hold=%u "
         "active=%d relaxed=%d\n",
         state->relaxed_blocks, CONFIG_TESTING_TITAN_ASR_ADAPTIVE_POWER,
         CONFIG_TESTING_TITAN_ASR_ADAPTIVE_HOLD,
         CONFIG_TESTING_TITAN_ASR_ECHO_SUPPRESS_ACTIVE,
         CONFIG_TESTING_TITAN_ASR_ADAPTIVE_DB);
#endif
  if (state->dump >= 0)
    {
      close(state->dump);
    }

  /* All Speex allocations belong to this arena; no per-buffer free needed. */

  free(state->arena);
  free(state);
  g_frontend = NULL;
}
