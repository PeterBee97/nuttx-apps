/****************************************************************************
 * apps/testing/titan_asr/titan_asr_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <nuttx/clock.h>
#include "asr_stream.h"
#include "asr_resample.h"
#include "board_metadata.h"
#ifdef CONFIG_TESTING_TITAN_ASR_DUPLEX
#  include "asr_frontend.h"
#endif
#ifdef CONFIG_TESTING_TITAN_DEMO
#  include "titan_demo.h"
#endif
#ifdef CONFIG_TESTING_TITAN_ASR_MVE
#  include "asr_mve.h"
#endif
#ifdef CONFIG_TESTING_TITAN_ASR_OPT
#  include "asr_opt.h"
void asr_bench_kernels(const int8_t *, float, int, unsigned int);
#endif

/****************************************************************************
 * Public Data
 ****************************************************************************/

extern const uint8_t board_lm[];
extern const uint8_t board_lm_end[];
extern const int8_t board_fixtures[];

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int board_model_init(void);
void board_model_close(void);
size_t board_model_arena(bool wake);
int board_model_run(bool wake, const int8_t *, size_t, int8_t *, size_t,
                    uint64_t *, uint32_t *);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct asr_stream_s state;
static uint64_t total_cycles;
static uint32_t last_us;
static bool dump_logits;
static int16_t (*queue)[160];
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static sem_t queue_ready;
static unsigned queue_read;
static unsigned queue_write;
static unsigned queue_count;
static unsigned queue_peak;
static bool queue_done;
static int worker_error;
static bool capture_only;
#ifdef CONFIG_TESTING_TITAN_DEMO
static bool demo;
static pthread_mutex_t model_lock = PTHREAD_MUTEX_INITIALIZER;
#endif
#ifdef CONFIG_TESTING_TITAN_ASR_SNAPSHOT_TRACE
static bool snapshot_trace;
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void *worker(void *argument)
{
  int16_t mono[160];
  uint64_t elapsed = 0;
  unsigned int consumed = 0;

  for (; ; )
    {
      while (sem_wait(&queue_ready) < 0 && errno == EINTR)
        {
        }

      pthread_mutex_lock(&queue_lock);
      if (queue_count == 0 && queue_done)
        {
          pthread_mutex_unlock(&queue_lock);
          break;
        }

      if (queue_count == 0)
        {
          pthread_mutex_unlock(&queue_lock);
          continue;
        }

      memcpy(mono, queue[queue_read++ %
                         CONFIG_TESTING_TITAN_ASR_QUEUE_BLOCKS],
             sizeof(mono));
      queue_count--;
      pthread_mutex_unlock(&queue_lock);
      if (worker_error == 0 && !capture_only)
        {
          uint32_t start = perf_gettime();

          worker_error = asr_stream_feed(&state, mono, 160);
#ifdef CONFIG_TESTING_TITAN_DEMO
          if (demo && worker_error < 0)
            {
              titan_demo_stop();
            }

#endif
          elapsed += (uint32_t)perf_gettime() - start;
          consumed++;
        }
    }

  printf("ASR_WORKER blocks=%u elapsed_us=%" PRIu64 "\n",
         consumed, elapsed * 1000000 / perf_getfreq());
  return NULL;
}

static int model(bool wake, const int8_t *features, size_t bytes,
                 int8_t *output, size_t capacity, void *argument)
{
  uint64_t cycles;
  int ret = board_model_run(wake, features, bytes, output, capacity,
                            &cycles, &last_us);

  if (ret == 0)
    {
      total_cycles += cycles;
    }

#ifdef CONFIG_TESTING_TITAN_ASR_SNAPSHOT_TRACE
  if (ret == 0 && !wake && snapshot_trace)
    {
      const void *buffers[] =
        {
          features, output
        };

      size_t sizes[] =
        {
          bytes, capacity
        };

      const char *suffixes[] =
        {
          "fea", "log"
        };

      char path[80];
      unsigned int i;

      for (i = 0; i < 2; i++)
        {
          snprintf(path, sizeof(path), "/ram/%08" PRIu32 ".%s",
                   state.sequence, suffixes[i]);
          FILE *file = fopen(path, "wb");

          if (file == NULL)
            {
              return -errno;
            }

          size_t written = fwrite(buffers[i], 1, sizes[i], file);
          int closed = fclose(file);

          if (written != sizes[i] || closed != 0)
            {
              return -EIO;
            }

          printf("ASR_SAVED path=%s bytes=%zu\n", path, written);
        }
    }
#endif

  return ret;
}

static void event(const struct titan_intent_event_s *e,
                  const struct asr_result_s *r, void *argument)
{
  printf("ASR_EVENT kind=%u intent=%u result=%u text=%s confidence=%.4f\n",
         e->kind, e->intent, e->result,
         r ? r->text : "", r ? r->confidence : 0.0);
#ifdef CONFIG_TESTING_TITAN_DEMO
  if (demo)
    {
      titan_demo_event(e);
    }

#endif
}

static int decode(const int8_t *features, unsigned index)
{
  uint64_t cycles;
  uint32_t micros;
#ifdef CONFIG_TESTING_TITAN_ASR_OPT
  int ret = 0;

  cycles = 0;
  micros = 0;
  if (features != NULL)
    {
      ret = board_model_run(false, features, 7960, state.logits, 30300,
                            &cycles, &micros);
    }
#else
  int ret = board_model_run(false, features, 7960, state.logits, 30300,
                            &cycles, &micros);
#endif
  if (ret < 0)
    {
      return ret;
    }

  uint64_t hash = UINT64_C(14695981039346656037);

  for (unsigned i = 0; i < sizeof(state.logits); i++)
    {
      hash = (hash ^ (uint8_t)state.logits[i]) * UINT64_C(1099511628211);
    }

  printf("ASR_LOGITS index=%u bytes=%zu fnv64=%016" PRIx64 "\n",
         index, sizeof(state.logits), hash);
  if (dump_logits)
    {
      printf("ASR_HEX_BEGIN\n");
      for (unsigned i = 0; i < sizeof(state.logits); i++)
        {
          printf("%02x", (uint8_t)state.logits[i]);
          if (i % 64 == 63)
            {
              printf("\n");
            }
        }

      printf("\nASR_HEX_END\n");
    }

  uint32_t start = perf_gettime();

  ret = asr_ctc_start(&state.workspace.decoder, &state.lm, .4, 1, .5);
  for (unsigned i = 0; ret == 0 && i < 100; i++)
    {
      ret = asr_ctc_frame_int8(&state.workspace.decoder,
                              state.logits + i * 303,
                              MODEL1_OUTPUT_SCALE, MODEL1_OUTPUT_ZERO);
    }

  if (ret < 0)
    {
      return ret;
    }

  uint32_t search_end = perf_gettime();

  memset(&state.transcript, 0, sizeof(state.transcript));
  ret = asr_ctc_finish(&state.workspace.decoder,
                       state.transcript.tokens, 100, NULL);
  if (ret < 0)
    {
      return ret;
    }

  state.transcript.length = ret;
  uint32_t finish_end = perf_gettime();

  ret = asr_postprocess_int8(&state.workspace.alignment, state.logits, 100,
                            MODEL1_OUTPUT_SCALE, MODEL1_OUTPUT_ZERO,
                            &state.transcript, 0);
  uint32_t elapsed = (uint32_t)perf_gettime() - start;
  uint32_t decode_us = ((uint64_t)elapsed * 1000000) / perf_getfreq();

  printf("ASR_PROFILE index=%u search_us=%" PRIu32
         " finish_us=%" PRIu32 " alignment_us=%" PRIu32 "\n",
         index,
         (uint32_t)((uint64_t)(search_end - start) *
                    1000000 / perf_getfreq()),
         (uint32_t)((uint64_t)(finish_end - search_end) *
                    1000000 / perf_getfreq()),
         (uint32_t)((uint64_t)(start + elapsed - finish_end) *
                    1000000 / perf_getfreq()));
  printf("ASR_FIXED index=%u gold=%s text=%s intent=%d confidence=%.4f "
         "npu_cycles=%" PRIu64 " inference_us=%" PRIu32
         " decode_us=%" PRIu32 "\n",
         index,
#if defined(CONFIG_TESTING_TITAN_ASR_OPT) || \
    defined(CONFIG_TESTING_TITAN_ASR_SNAPSHOT_TRACE)
         index < BOARD_FIXTURES ? fixture_labels[index] : "AUDIT",
#else
         fixture_labels[index],
#endif
         state.transcript.text, state.transcript.intent,
         state.transcript.confidence, cycles, micros, decode_us);
#ifdef CONFIG_TESTING_TITAN_ASR_MVE
  printf("ASR_COMPARE mode=%s index=%u confidence=%.17g minimum=%.17g "
         "unknown=%u length=%u tokens=",
         asr_mve_enabled() ? "mve" : "scalar", index,
         state.transcript.confidence,
         state.transcript.minimum_confidence,
         state.transcript.unknown_characters,
         state.transcript.length);
  for (unsigned int i = 0; i < state.transcript.length; i++)
    {
      printf("%u:%u,", state.transcript.tokens[i],
             state.transcript.frames[i]);
    }

  printf("\n");
#endif
#ifdef CONFIG_TESTING_TITAN_ASR_OPT
  hash = UINT64_C(14695981039346656037);
  const uint8_t *result_bytes = (const uint8_t *)&state.transcript;

  for (unsigned int byte = 0; byte < sizeof(state.transcript); byte++)
    {
      hash = (hash ^ result_bytes[byte]) * UINT64_C(1099511628211);
    }

  printf("ASR_RESULT_HASH fnv64=%016" PRIx64 "\n", hash);
#endif
  fflush(stdout);
#ifdef CONFIG_TESTING_TITAN_ASR_OPT
  if (g_asr_profile_enabled)
    {
      for (unsigned int part = 0; part < ASR_PROFILE_COUNT; part++)
        {
          printf("ASR_SECTION part=%u calls=%" PRIu32 " cycles=%" PRIu64
                 " us=%" PRIu64 "\n", part, g_asr_profile_calls[part],
                 g_asr_profile_cycles[part],
                 g_asr_profile_cycles[part] * 1000000 / perf_getfreq());
        }
    }

#endif
  return ret;
}

#ifdef CONFIG_TESTING_TITAN_ASR_MVE
static void check_kernels(unsigned int index)
{
  float expanded[ASR_CTC_CLASSES];
  float exponentials[ASR_CTC_CLASSES];
  double max_relative = 0;
  double max_absolute = 0;
  unsigned int mismatches = 0;
  float maximum;
  float expected;
  double reference;
  double difference;
  double relative;

  for (unsigned int frame = 0; frame < ASR_CTC_FRAMES; frame++)
    {
      const int8_t *input = state.logits + frame * ASR_CTC_CLASSES;

      maximum = -INFINITY;
      asr_mve_expand(input, MODEL1_OUTPUT_SCALE, MODEL1_OUTPUT_ZERO,
                     expanded, ASR_CTC_CLASSES);
      for (unsigned int token = 0; token < ASR_CTC_CLASSES; token++)
        {
          expected = (float)(input[token] - MODEL1_OUTPUT_ZERO) *
                     MODEL1_OUTPUT_SCALE;
          mismatches += memcmp(&expected, &expanded[token],
                               sizeof(expected)) != 0;
          if (maximum < expected)
            {
              maximum = expected;
            }
        }

      asr_mve_exponentials(expanded, maximum, exponentials, ASR_CTC_CLASSES);
      for (unsigned int token = 0; token < ASR_CTC_CLASSES; token++)
        {
          reference = exp((double)expanded[token] - maximum);
          difference = fabs((double)exponentials[token] - reference);
          relative = reference > 0 ? difference / reference : 0;
          if (max_absolute < difference)
            {
              max_absolute = difference;
            }

          if (max_relative < relative)
            {
              max_relative = relative;
            }
        }
    }

  printf("ASR_KERNEL index=%u expand_mismatches=%u "
         "exp_max_abs=%.17g exp_max_rel=%.17g\n",
         index, mismatches, max_absolute, max_relative);
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char **argv)
{
#ifdef CONFIG_TESTING_TITAN_DEMO
  if (argc > 1 && strcmp(argv[1], "demo-stop") == 0)
    {
      titan_demo_stop();
      return 0;
    }

  if (argc > 1 && strcmp(argv[1], "demo-status") == 0)
    {
      titan_demo_status();
      return 0;
    }

  if (pthread_mutex_trylock(&model_lock) != 0)
    {
      printf("ASR_BUSY\n");
      return 1;
    }

#ifdef CONFIG_TESTING_TITAN_DEMO_STORAGE
  if (argc > 1 && (strcmp(argv[1], "demo-boot") == 0 ||
                   strcmp(argv[1], "demo-mount") == 0))
    {
      int storage = titan_storage_start();

      if (storage < 0 || strcmp(argv[1], "demo-mount") == 0)
        {
          pthread_mutex_unlock(&model_lock);
          return storage < 0 ? 1 : 0;
        }

      argv[1] = "demo";
    }
#endif

  demo = argc > 1 && (strcmp(argv[1], "demo") == 0 ||
                     strcmp(argv[1], "demo-replay") == 0);
  bool demo_opened = false;
#endif
  int ret = board_model_init();

  if (ret < 0)
    {
      printf("ASR_INIT_ERROR %d\n", ret);
#ifdef CONFIG_TESTING_TITAN_DEMO
      pthread_mutex_unlock(&model_lock);
#endif
      return 1;
    }

  struct asr_stream_config_s config =
    {
      .policy =
        {
          8000, 1000, WAKE_SCORE, WAKE_MARGIN, 0, 0
        },
      .alpha = .4, .insertion_bonus = 1, .blank_penalty = .5,
      .threshold = CONFIG_TESTING_TITAN_ASR_MIN_CONFIDENCE / 100.0,
      .asr_scale = MODEL1_OUTPUT_SCALE, .asr_zero = MODEL1_OUTPUT_ZERO,
      .wake_scale_q20 = WAKE_SCALE_Q20, .model = model, .event = event
    };

#ifdef CONFIG_TESTING_TITAN_DEMO
  config.strict_wake = demo;
#endif
  ret = asr_stream_init(&state, &config, board_lm,
                        board_lm_end - board_lm, 1, false);
  printf("ASR_READY ret=%d wake_arena=%zu asr_arena=%zu context=%zu\n",
         ret, board_model_arena(true),
         board_model_arena(false), sizeof(state));
  fflush(stdout);
#ifdef CONFIG_TESTING_TITAN_ASR_SNAPSHOT_TRACE
  if (ret == 0 && argc > 2 && strcmp(argv[1], "features") == 0)
    {
      int fd = open(argv[2], O_RDONLY);
      size_t filled = 0;

      ret = fd < 0 ? -errno : 0;
      while (ret == 0 && filled < sizeof(state.snapshot))
        {
          ssize_t size = read(fd, state.snapshot + filled,
                              sizeof(state.snapshot) - filled);

          if (size <= 0)
            {
              ret = size < 0 ? -errno : -EIO;
              break;
            }

          filled += size;
        }

      if (fd >= 0)
        {
          close(fd);
        }

      if (ret == 0)
        {
#ifdef CONFIG_TESTING_TITAN_ASR_OPT
          g_asr_opt_mask = 14;
          asr_mve_enable(false);
#endif
          snapshot_trace = true;
          ret = model(false, state.snapshot, sizeof(state.snapshot),
                      state.logits, sizeof(state.logits), NULL);
        }

      if (ret == 0)
        {
#ifdef CONFIG_TESTING_TITAN_ASR_OPT
          ret = decode(NULL, BOARD_FIXTURES);
#else
          ret = decode(state.snapshot, BOARD_FIXTURES);
#endif
        }

      goto done;
    }

#endif
#ifdef CONFIG_TESTING_TITAN_ASR_DUPLEX
  if (ret == 0 && argc > 4 &&
      (strcmp(argv[1], "replay") == 0
#ifdef CONFIG_TESTING_TITAN_DEMO
       || strcmp(argv[1], "demo-replay") == 0
#endif
      ))
    {
#ifdef CONFIG_TESTING_TITAN_ASR_SNAPSHOT_TRACE
      snapshot_trace = true;
#endif
      int16_t input[480];
      int16_t mono[160];
      unsigned int blocks = strtoul(argv[3], NULL, 10);
      int fd = open(argv[2], O_RDONLY);
      bool replay_dump = argc > 5 && atoi(argv[5]) != 0;

#ifdef CONFIG_TESTING_TITAN_DEMO
      if (demo)
        {
          replay_dump = false;
        }

#endif
      ret = ret < 0 ? ret : fd < 0 ? -errno :
            asr_frontend_open(strtoul(argv[4], NULL, 10), replay_dump);
#ifdef CONFIG_TESTING_TITAN_DEMO
      if (ret == 0 && demo)
        {
          ret = titan_demo_open(argc > 5 ? argv[5] :
                                CONFIG_TESTING_TITAN_DEMO_DIRECTORY);
          demo_opened = ret == 0;
        }

#endif
#ifdef CONFIG_TESTING_TITAN_ASR_SNAPSHOT_TRACE
      if (ret == 0 && argc > 7)
        {
          ret = asr_frontend_diagnostic(atoi(argv[6]), atoi(argv[7]));
        }

#endif
#ifdef CONFIG_TESTING_TITAN_ASR_OPT
      g_asr_opt_mask = 14;
      asr_mve_enable(false);
#endif
#ifdef CONFIG_TESTING_TITAN_DEMO
      if (!demo)
#endif
        {
          asr_stream_authorize_window(&state);
        }

      for (unsigned int block = 0; block < blocks && ret == 0; block++)
        {
          size_t filled = 0;

          while (filled < sizeof(input))
            {
              ssize_t size = read(fd, (uint8_t *)input + filled,
                                  sizeof(input) - filled);

              if (size <= 0)
                {
                  ret = size < 0 ? -errno : -EIO;
                  break;
                }

              filled += size;
            }

          if (ret == 0)
            {
              ret = asr_frontend_replay(input, mono);
              if (ret > 0)
                {
#ifdef CONFIG_TESTING_TITAN_DEMO
                  if (demo)
                    {
                      titan_demo_pcm(mono);
                      usleep(10000);
                    }

#endif
                  ret = asr_stream_feed(&state, mono, 160);
                }
            }
        }

      if (fd >= 0)
        {
          close(fd);
        }

      asr_frontend_close();
      printf("ASR_REPLAY ret=%d blocks=%u asr=%" PRIu64
             " failures=%" PRIu64 " text=%s intent=%d\n",
             ret, blocks, state.asr_invokes, state.failures,
             state.transcript.text, state.transcript.intent);
      goto done;
    }

#endif
  if (ret == 0 && argc > 1 &&
      (strcmp(argv[1], "mic") == 0
#ifdef CONFIG_TESTING_TITAN_DEMO
       || demo
#endif
      ))
    {
      unsigned seconds = argc > 2 ? strtoul(argv[2], NULL, 10) : 20;

#ifdef CONFIG_TESTING_TITAN_DEMO
      if (demo)
        {
          seconds = argc > 3 ? strtoul(argv[3], NULL, 10) : 0;
          ret = seconds > 86400 ? -EINVAL : 0;
        }

#endif
      if (
#ifdef CONFIG_TESTING_TITAN_DEMO
          !demo &&
#endif
          (seconds < 1 || seconds > 60))
        {
          ret = -EINVAL;
        }

#ifdef CONFIG_TESTING_TITAN_ASR_OPT
      g_asr_opt_mask = 14;
      asr_mve_enable(false);
#endif
      const char *device = "/dev/pdm0";
      unsigned channels = 2;
#ifdef CONFIG_TESTING_TITAN_ASR_DUPLEX
      bool duplex = argc > 3;

#ifdef CONFIG_TESTING_TITAN_DEMO
      if (demo)
        {
          duplex = true;
        }

#endif
      if (duplex && ret == 0)
        {
#ifdef CONFIG_TESTING_TITAN_DEMO
          unsigned int mode = argc > 3 ? strtoul(argv[3], NULL, 10) : 2;
          bool dump = argc > 4 && atoi(argv[4]) != 0;

          if (demo)
            {
              mode = argc > 4 ? strtoul(argv[4], NULL, 10) :
                     CONFIG_TESTING_TITAN_DEMO_FRONTEND_MODE;
              dump = false;
            }

          if (demo && mode > 2)
            {
              ret = -EINVAL;
            }
          else
            {
              ret = asr_frontend_open(mode, dump);
            }

#else
          ret = asr_frontend_open(strtoul(argv[3], NULL, 10),
                                 argc > 4 && atoi(argv[4]) != 0);
#endif
          device = "/dev/pdmref";
          channels = 3;
        }

#ifdef CONFIG_TESTING_TITAN_DEMO
      if (ret == 0 && demo)
        {
          /* Allocate the realtime frontend before the larger media queue,
           * so AEC can use the internal SRAM region.
           */

          ret = titan_demo_open(argc > 2 ? argv[2] :
                                CONFIG_TESTING_TITAN_DEMO_DIRECTORY);
          demo_opened = ret == 0;
        }

#endif
      int16_t pcm[480];
#else
      int16_t pcm[320];
#endif
      int fd = ret == 0 ? open(device, O_RDONLY) : -1;

      if (fd < 0 && ret == 0)
        {
          ret = -errno;
        }

      int16_t converted[160];
      unsigned converted_count = 0;
      struct asr_resample_s resampler =
        {
          0
        };

      size_t filled = 0;
      uint64_t samples = 0;

#ifdef CONFIG_TESTING_TITAN_DEMO
      if (!demo)
#endif
        {
          asr_stream_authorize_window(&state);
        }

      queue_read = queue_write = queue_count = queue_peak = 0;
      queue = calloc(CONFIG_TESTING_TITAN_ASR_QUEUE_BLOCKS, sizeof(*queue));
      if (queue == NULL && ret == 0)
        {
          ret = -ENOMEM;
        }

      queue_done = false;
      worker_error = 0;
      capture_only = argc > 5 && atoi(argv[5]) != 0;
#ifdef CONFIG_TESTING_TITAN_DEMO
      if (demo)
        {
          capture_only = false;
        }

#endif
#ifdef CONFIG_TESTING_TITAN_ASR_DUPLEX
      struct sched_param capture_priority;

      sched_getparam(0, &capture_priority);
      struct sched_param realtime_priority =
        {
          .sched_priority = CONFIG_TESTING_TITAN_ASR_CAPTURE_PRIORITY
        };

      if (duplex)
        {
          sched_setparam(0, &realtime_priority);
        }

#endif
      sem_init(&queue_ready, 0, 0);
      pthread_t thread;
      pthread_attr_t attr;

      pthread_attr_init(&attr);
      pthread_attr_setstacksize(&attr, 16384);
      struct sched_param priority =
        {
          .sched_priority = CONFIG_TESTING_TITAN_ASR_DECODE_PRIORITY
        };

      pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
      pthread_attr_setschedparam(&attr, &priority);
      int created = ret == 0 ?
                    pthread_create(&thread, &attr, worker, NULL) : -1;

      pthread_attr_destroy(&attr);
      if (created != 0 && ret == 0)
        {
          ret = -created;
        }

#ifdef CONFIG_TESTING_TITAN_DEMO
      while (ret == 0 &&
             (seconds == 0 || samples < (uint64_t)seconds * 16000))
#else
      while (ret == 0 && samples < seconds * 16000)
#endif
        {
#ifdef CONFIG_TESTING_TITAN_DEMO
          if (demo && titan_demo_stopping())
            {
              break;
            }

#endif
          struct pollfd pfd =
            {
              .fd = fd, .events = POLLIN
            };

          int ready = poll(&pfd, 1, 2000);

          if (ready <= 0)
            {
              ret = ready < 0 ? -errno : -ETIMEDOUT;
              break;
            }

          size_t bytes = channels * 160 * sizeof(int16_t);
          ssize_t n = read(fd, (uint8_t *)pcm + filled, bytes - filled);

          if (n <= 0 || n % (channels * 2))
            {
              ret = n < 0 ? -errno : -EIO;
              break;
            }

          filled += n;
          if (filled < bytes)
            {
              continue;
            }

#ifdef CONFIG_TESTING_TITAN_ASR_DUPLEX
          if (duplex)
            {
              int produced = asr_frontend_push(pcm, converted);

              filled = 0;
              if (produced < 0)
                {
                  ret = produced;
                  break;
                }

              if (produced == 0)
                {
                  continue;
                }

#ifdef CONFIG_TESTING_TITAN_DEMO
              if (demo)
                {
                  titan_demo_pcm(converted);
                }

#endif
              pthread_mutex_lock(&queue_lock);
              if (queue_count == CONFIG_TESTING_TITAN_ASR_QUEUE_BLOCKS)
                {
                  pthread_mutex_unlock(&queue_lock);
                  ret = -ENOSPC;
                  break;
                }

              memcpy(queue[queue_write++ %
                           CONFIG_TESTING_TITAN_ASR_QUEUE_BLOCKS], converted,
                     sizeof(converted));
              queue_count++;
              if (queue_peak < queue_count)
                {
                  queue_peak = queue_count;
                }

              pthread_mutex_unlock(&queue_lock);
              sem_post(&queue_ready);
              samples += 160;
              continue;
            }

#endif
          for (unsigned i = 0; i < 160; i++)
            {
#ifdef CONFIG_TESTING_TITAN_ASR_MIC_MEAN
              int16_t input = ((int32_t)pcm[2 * i] + pcm[2 * i + 1]) / 2;
#else
              int16_t input = pcm[2 * i];
#endif
              if (!asr_resample(&resampler, input,
                                CONFIG_TESTING_TITAN_ASR_MIC_RATE,
                                &converted[converted_count]))
                {
                  continue;
                }

              converted_count++;
              if (converted_count < 160)
                {
                  continue;
                }

              pthread_mutex_lock(&queue_lock);
              if (queue_count == CONFIG_TESTING_TITAN_ASR_QUEUE_BLOCKS)
                {
                  pthread_mutex_unlock(&queue_lock);
                  ret = -ENOSPC;
                  break;
                }

              int16_t *mono = queue[queue_write++ %
                                   CONFIG_TESTING_TITAN_ASR_QUEUE_BLOCKS];

              memcpy(mono, converted, sizeof(converted));
              converted_count = 0;
              queue_count++;
              if (queue_peak < queue_count)
                {
                  queue_peak = queue_count;
                }

              pthread_mutex_unlock(&queue_lock);
              sem_post(&queue_ready);
              samples += 160;
            }

          filled = 0;
        }

      if (fd >= 0)
        {
          close(fd);
        }

#ifdef CONFIG_TESTING_TITAN_ASR_DUPLEX
      if (duplex)
        {
          sched_setparam(0, &capture_priority);
        }

      if (duplex)
        {
          asr_frontend_close();
        }

#endif
      pthread_mutex_lock(&queue_lock);
      queue_done = true;
      pthread_mutex_unlock(&queue_lock);
      sem_post(&queue_ready);
      if (created == 0)
        {
          pthread_join(thread, NULL);
        }

      free(queue);
      queue = NULL;
      sem_destroy(&queue_ready);
      if (ret == 0)
        {
          ret = worker_error;
        }

      printf("ASR_TRANSCRIPT text=%s intent=%d markers=%" PRIu64
             " queue_peak=%u\n",
             state.transcript.text, state.transcript.intent,
             state.marker_count, queue_peak);
      printf("ASR_MIC ret=%d samples=%" PRIu64 " wake=%" PRIu64
             " asr=%" PRIu64 " failures=%" PRIu64 " cycles=%" PRIu64 "\n",
             ret, samples, state.wake_invokes, state.asr_invokes,
             state.failures, total_cycles);
    }
  else if (ret == 0)
    {
#ifdef CONFIG_TESTING_TITAN_ASR_OPT
      if (argc > 1 && strcmp(argv[1], "micro") == 0)
        {
          for (unsigned int index = 0; index < BOARD_FIXTURES; index++)
            {
              g_asr_opt_mask = 15;
              asr_mve_enable(true);
              ret = decode(board_fixtures + index * 7960, index);
              if (ret < 0)
                {
                  break;
                }

              asr_bench_kernels(state.logits, MODEL1_OUTPUT_SCALE,
                                MODEL1_OUTPUT_ZERO, index);
            }

          goto done;
        }

      if (argc > 1 && strcmp(argv[1], "audit") == 0)
        {
          unsigned int count = argc > 2 ? strtoul(argv[2], NULL, 10) : 32;
          int fd = open("/dev/ttyACM0", O_RDONLY);

          if (fd < 0 || count > 256)
            {
              ret = -EINVAL;
            }

          for (unsigned int index = 0; index < count && ret == 0; index++)
            {
              size_t filled = 0;

              printf("ASR_INPUT index=%u\n", index);
              fflush(stdout);
              while (filled < sizeof(state.logits))
                {
                  ssize_t size = read(fd, state.logits + filled,
                                      sizeof(state.logits) - filled);

                  if (size <= 0)
                    {
                      ret = size < 0 ? -errno : -EIO;
                      break;
                    }

                  filled += size;
                }

              static const unsigned int masks[] =
                {
                  0, 14, 15
                };

              for (unsigned int mode = 0; mode < 3 && ret == 0; mode++)
                {
                  g_asr_opt_mask = masks[mode];
                  asr_mve_enable((masks[mode] & 1) != 0);
                  printf("ASR_DEEP round=0 mask=%u index=%u profile=0\n",
                         masks[mode], index);
                  ret = decode(NULL, index + BOARD_FIXTURES);
                }
            }

          if (fd >= 0)
            {
              close(fd);
            }

          goto done;
        }

      if (argc > 1 && strcmp(argv[1], "deep") == 0)
        {
          static const unsigned int masks[] =
            {
              0, 1, 2, 4, 6, 7, 8, 14, 15
            };

          unsigned int repeats = argc > 2 ? strtoul(argv[2], NULL, 10) : 3;

          g_asr_profile_enabled = argc > 3 && atoi(argv[3]) != 0;
          if (repeats < 1 || repeats > 5)
            {
              ret = -EINVAL;
            }

          for (unsigned int round = 0; round < repeats && ret == 0; round++)
            {
              for (unsigned int index = 0;
                   index < BOARD_FIXTURES && ret == 0; index++)
                {
                  for (unsigned int mode = 0; mode < 9 && ret == 0; mode++)
                    {
                      unsigned int mask = masks[(mode + round) % 9];

                      g_asr_opt_mask = mask;
                      asr_mve_enable((mask & 1) != 0);
                      memset(g_asr_profile_cycles, 0,
                             sizeof(g_asr_profile_cycles));
                      memset(g_asr_profile_calls, 0,
                             sizeof(g_asr_profile_calls));
                      printf("ASR_DEEP round=%u mask=%u index=%u "
                             "profile=%u\n",
                             round, mask, index, g_asr_profile_enabled);
                      ret = decode(board_fixtures + index * 7960, index);
                    }
                }
            }

          g_asr_opt_mask = 0;
          g_asr_profile_enabled = false;
          asr_mve_enable(false);
          goto done;
        }

#endif
#ifdef CONFIG_TESTING_TITAN_ASR_MVE
      if (argc > 1 && strcmp(argv[1], "bench") == 0)
        {
          unsigned int repeats = argc > 2 ? strtoul(argv[2], NULL, 10) : 5;

          if (repeats < 1 || repeats > 10)
            {
              ret = -EINVAL;
            }

          for (unsigned int round = 0; round < repeats && ret == 0; round++)
            {
              for (unsigned int index = 0;
                   index < BOARD_FIXTURES && ret == 0; index++)
                {
                  for (unsigned int mode = 0; mode < 2 && ret == 0; mode++)
                    {
                      asr_mve_enable((mode ^ (round & 1)) != 0);
                      printf("ASR_BENCH round=%u mode=%s index=%u\n",
                             round, asr_mve_enabled() ? "mve" : "scalar",
                             index);
                      ret = decode(board_fixtures + index * 7960, index);
                    }

                  if (round == 0 && ret == 0)
                    {
                      check_kernels(index);
                    }
                }
            }

          asr_mve_enable(true);
          goto done;
        }

#endif
      dump_logits = argc > 1 && strcmp(argv[1], "dump") == 0;
      unsigned first = dump_logits && argc > 2 ?
                       strtoul(argv[2], NULL, 10) : 0;
      unsigned end = dump_logits ? first + 1 : BOARD_FIXTURES;

      if (end > BOARD_FIXTURES)
        {
          ret = -EINVAL;
        }

      for (unsigned i = first; i < end && ret == 0; i++)
        {
          ret = decode(board_fixtures + i * 7960, i);
        }
    }

#if defined(CONFIG_TESTING_TITAN_ASR_MVE) || \
    defined(CONFIG_TESTING_TITAN_ASR_DUPLEX) || \
    defined(CONFIG_TESTING_TITAN_ASR_SNAPSHOT_TRACE)
done:
#endif
  printf("ASR_DONE ret=%d\n", ret);
#ifdef CONFIG_TESTING_TITAN_DEMO
  if (demo_opened)
    {
      titan_demo_close();
    }

#endif
  board_model_close();
#ifdef CONFIG_TESTING_TITAN_DEMO
  demo = false;
  pthread_mutex_unlock(&model_lock);
#endif
  return ret < 0 ? 1 : 0;
}
