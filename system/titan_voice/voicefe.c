/****************************************************************************
 * apps/system/titan_voice/voicefe.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#if defined(CONFIG_SYSTEM_TITAN_VOICE_ASR_HTTP) || \
    defined(CONFIG_SYSTEM_TITAN_VOICE_INTENT_TELEMETRY)
#include <sys/socket.h>
#include <arpa/inet.h>
#endif
#include <sys/ioctl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include <nuttx/audio/pdm.h>
#include <nuttx/clock.h>
#include <audioutils/titan_voicefe.h>

#ifdef CONFIG_SYSTEM_TITAN_VOICE_INTENT
#include "voicefe_intent.h"
#endif
#ifdef CONFIG_SYSTEM_TITAN_VOICE_INTENT_TELEMETRY
#include "voicefe_telemetry.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VF_PREROLL (CONFIG_SYSTEM_TITAN_VOICE_PREROLL_MS / 10)
#define VF_QUEUE   (VF_PREROLL + 51)
#define VF_IDLE    0
#define VF_LIVE    1
#define VF_END     2
#define VF_ABORT   3

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct voicefe_s
{
  pthread_t rt;
#ifdef CONFIG_SYSTEM_TITAN_VOICE_ASR_HTTP
  pthread_t net;
#endif
  sem_t wake;
  sem_t begin;
  atomic_bool stop;
  atomic_bool active;
  atomic_uint head;
  atomic_uint tail;
  atomic_uint session;
  atomic_uint blocks;
  atomic_uint utterances;
  atomic_uint failures;
  atomic_uint highwater;
  atomic_uint energy;
  atomic_uint noise;
  atomic_uint ended;
  atomic_uint latency;
  atomic_uint connect_ms;
#ifdef CONFIG_SYSTEM_TITAN_VOICE_NS
  struct voicefe_ns_s ns;
  atomic_uint ns_gain;
  atomic_uint ns_clipped;
  bool denoise;
#endif
#ifdef CONFIG_SYSTEM_TITAN_VOICE_PROFILE
  atomic_uint cycles;
  atomic_uint avgcycles;
  atomic_uint maxcycles;
#endif
  struct voicefe_dsp_s dsp;
  struct pdm_stats_s pdm;
  int16_t stereo[VOICEFE_FRAMES * 2];
  int16_t mono[VOICEFE_FRAMES];
#ifdef CONFIG_SYSTEM_TITAN_VOICE_ASR_HTTP
  int16_t preroll[VF_PREROLL][VOICEFE_FRAMES];
  int16_t queue[VF_QUEUE][VOICEFE_FRAMES];
#endif
#ifdef CONFIG_SYSTEM_TITAN_VOICE_INTENT
  uint64_t net_utterance;
  char boot[17];
  bool intent;
#endif
  char host[16];
  int port;
  int fd;
  bool running;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_pdm_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_result_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_result[1536];
static int g_result_error;
static struct voicefe_s g_voice;
static sem_t g_ready;
static sem_t g_done;
static int g_start_result;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int voicefe_read(int fd, int16_t *buffer)
{
  struct pollfd pfd;
  size_t offset = 0;
  ssize_t nread;
  int ret;

  pfd.fd = fd;
  pfd.events = POLLIN;
  while (offset < VOICEFE_FRAMES * 4)
    {
      if (atomic_load(&g_voice.stop))
        {
          return -ECANCELED;
        }

      ret = poll(&pfd, 1, 100);
      if (ret < 0 && errno == EINTR)
        {
          continue;
        }

      if (ret < 0)
        {
          return -errno;
        }

      if (ret == 0)
        {
          continue;
        }

      nread = read(fd, (char *)buffer + offset,
                   VOICEFE_FRAMES * 4 - offset);
      if (nread < 0 && (errno == EAGAIN || errno == EINTR))
        {
          continue;
        }

      if (nread <= 0)
        {
          return nread == 0 ? -EPIPE : -errno;
        }

      offset += nread;
    }

  return 0;
}

#ifdef CONFIG_SYSTEM_TITAN_VOICE_ASR_HTTP
static bool voicefe_put(const int16_t *pcm)
{
  unsigned int head = atomic_load(&g_voice.head);
  unsigned int tail = atomic_load(&g_voice.tail);
  unsigned int count = head - tail;

  if (count == VF_QUEUE)
    {
      atomic_store(&g_voice.session, VF_ABORT);
      atomic_fetch_add(&g_voice.failures, 1);
      sem_post(&g_voice.wake);
      return false;
    }

  memcpy(g_voice.queue[head % VF_QUEUE], pcm, VOICEFE_FRAMES * 2);
  atomic_store(&g_voice.head, head + 1);
  if (count + 1 > atomic_load(&g_voice.highwater))
    {
      atomic_store(&g_voice.highwater, count + 1);
    }

  sem_post(&g_voice.wake);
  return true;
}
#endif

static void *voicefe_rt(void *arg)
{
#ifdef CONFIG_SYSTEM_TITAN_VOICE_ASR_HTTP
  unsigned int prehead = 0;
  unsigned int precount = 0;
#endif
  unsigned int quiet = 0;
  unsigned int duration = 0;
  unsigned int speechcount = 0;
  unsigned int candidate = 0;
  unsigned int candidatequiet = 0;
  unsigned int expected;
#ifdef CONFIG_SYSTEM_TITAN_VOICE_ASR_HTTP
  unsigned int i;
#endif
#ifdef CONFIG_SYSTEM_TITAN_VOICE_INTENT
  uint64_t marker = 0;
  uint64_t marker_start = 0;
  uint64_t marker_count = 0;
  unsigned int marker_quiet = 0;
  uint32_t sequence = 0;
  bool marker_end;
#ifdef CONFIG_SYSTEM_TITAN_VOICE_ASR_HTTP
  bool upload_turn = false;
#endif
  bool marker_net_attempted = false;
#endif
  bool active = false;
  bool speech;
  bool upload = true;
  int ret;
#ifdef CONFIG_SYSTEM_TITAN_VOICE_PROFILE
  uint32_t started;
  uint32_t elapsed;
  uint64_t totalcycles = 0;
  uint32_t measured = 0;
#endif

  while (sem_wait(&g_voice.begin) < 0 && errno == EINTR)
    {
    }

  while (!atomic_load(&g_voice.stop))
    {
      ret = voicefe_read(g_voice.fd, g_voice.stereo);
      if (ret < 0)
        {
          atomic_store(&g_voice.stop, true);
          break;
        }

#ifdef CONFIG_SYSTEM_TITAN_VOICE_PROFILE
      started = perf_gettime();
#endif
      voicefe_process(&g_voice.dsp, g_voice.stereo, g_voice.mono);
      speech = voicefe_speech(&g_voice.dsp, g_voice.mono);
#ifndef CONFIG_SYSTEM_TITAN_VOICE_VAD
      speech = true;
#endif
      atomic_store(&g_voice.energy, g_voice.dsp.energy);
      atomic_store(&g_voice.noise, g_voice.dsp.noise);
      atomic_fetch_add(&g_voice.blocks, 1);
#ifdef CONFIG_SYSTEM_TITAN_VOICE_INTENT
      marker_end = false;
      if (g_voice.intent)
        {
          if (speech && marker == 0)
            {
              marker = ++marker_count;
              marker_start = (uint64_t)sequence * VOICEFE_FRAMES;
              marker_net_attempted = false;
#ifdef CONFIG_SYSTEM_TITAN_VOICE_ASR_HTTP
              upload_turn = voicefe_intent_upload();
#endif
            }

          marker_quiet = speech ? 0 : marker_quiet + 1;
          marker_end = marker != 0 && marker_quiet >= 80;
          voicefe_intent_put(g_voice.mono, sequence, marker, marker_start,
                            speech, marker_end);
        }

      sequence++;
#endif
#ifdef CONFIG_SYSTEM_TITAN_VOICE_NS
      if (g_voice.denoise)
        {
          voicefe_ns_process(&g_voice.ns, g_voice.mono);
          atomic_store(&g_voice.ns_gain, g_voice.ns.gain_q15);
          atomic_store(&g_voice.ns_clipped, g_voice.ns.clipped);
        }
#endif

      if (!active)
        {
          if (speech)
            {
              candidate++;
              candidatequiet = 0;
            }
          else if (++candidatequiet >= 10)
            {
              candidate = 0;
            }
        }

      if (!active && candidate >= 30
#ifdef CONFIG_SYSTEM_TITAN_VOICE_INTENT
          && (!g_voice.intent || !marker_net_attempted)
#endif
          )
        {
          active = true;
          atomic_store(&g_voice.active, true);
          quiet = 0;
          duration = 0;
          speechcount = candidate;
          candidate = 0;
          atomic_fetch_add(&g_voice.utterances, 1);
          expected = VF_IDLE;
#ifdef CONFIG_SYSTEM_TITAN_VOICE_ASR_HTTP
#ifdef CONFIG_SYSTEM_TITAN_VOICE_INTENT
          marker_net_attempted = g_voice.intent;
          upload = !g_voice.intent || upload_turn;
          if (upload && atomic_load(&g_voice.session) == VF_IDLE)
            {
              g_voice.net_utterance = marker;
            }

#endif
          if (upload && atomic_compare_exchange_strong(&g_voice.session,
                                                        &expected, VF_LIVE))
            {
              for (i = 0; i < precount; i++)
                {
                  voicefe_put(g_voice.preroll[
                    (prehead + VF_PREROLL - precount + i) % VF_PREROLL]);
                }
            }
          else if (upload)
            {
              atomic_fetch_add(&g_voice.failures, 1);
            }
#else
          (void)expected;
          (void)upload;
#endif
        }

      if (active)
        {
          duration++;
          speechcount += speech;
          quiet = speech ? 0 : quiet + 1;
#ifdef CONFIG_SYSTEM_TITAN_VOICE_ASR_HTTP
          if (atomic_load(&g_voice.session) == VF_LIVE)
            {
              voicefe_put(g_voice.mono);
            }
#endif

          if (quiet >= CONFIG_SYSTEM_TITAN_VOICE_HANGOVER_MS / 10 ||
              duration >= CONFIG_SYSTEM_TITAN_VOICE_MAX_UTTERANCE_MS / 10)
            {
              active = false;
              atomic_store(&g_voice.ended, clock_systime_ticks());
              expected = VF_LIVE;
              atomic_compare_exchange_strong(&g_voice.session, &expected,
                speechcount >= 30 ? VF_END : VF_ABORT);
              atomic_store(&g_voice.active, false);
              sem_post(&g_voice.wake);
            }
        }

#ifdef CONFIG_SYSTEM_TITAN_VOICE_ASR_HTTP
      memcpy(g_voice.preroll[prehead], g_voice.mono, VOICEFE_FRAMES * 2);
      prehead = (prehead + 1) % VF_PREROLL;
      if (precount < VF_PREROLL)
        {
          precount++;
        }
#endif

#ifdef CONFIG_SYSTEM_TITAN_VOICE_INTENT
      if (marker_end)
        {
          marker = 0;
          candidate = 0;
          marker_net_attempted = false;
        }
#endif

#ifdef CONFIG_SYSTEM_TITAN_VOICE_PROFILE
      elapsed = (uint32_t)perf_gettime() - started;
      totalcycles += elapsed;
      measured++;
      atomic_store(&g_voice.cycles, elapsed);
      atomic_store(&g_voice.avgcycles, totalcycles / measured);
      if (elapsed > atomic_load(&g_voice.maxcycles))
        {
          atomic_store(&g_voice.maxcycles, elapsed);
        }
#endif
    }

  atomic_store(&g_voice.session, VF_ABORT);
  atomic_store(&g_voice.active, false);
  sem_post(&g_voice.wake);
  return NULL;
}

#ifdef CONFIG_SYSTEM_TITAN_VOICE_ASR_HTTP
static int voicefe_send(int fd, const void *data, size_t size)
{
  ssize_t sent;

  while (size > 0)
    {
      if (atomic_load(&g_voice.stop))
        {
          return -ECANCELED;
        }

      sent = send(fd, data, size, 0);
      if (sent < 0 && errno == EINTR)
        {
          continue;
        }

      if (sent <= 0)
        {
          return sent == 0 ? -EPIPE : -errno;
        }

      data = (const char *)data + sent;
      size -= sent;
    }

  return 0;
}

static int voicefe_connect(void)
{
  struct sockaddr_in addr;
  struct timeval timeout =
  {
    2, 0
  };

  char header[512];
  socklen_t length;
  int error;
  uint32_t started;
  int size;
  int fd;
  int ret;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    {
      return -errno;
    }

  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  fcntl(fd, F_SETFL, O_NONBLOCK);
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(g_voice.port);
  inet_pton(AF_INET, g_voice.host, &addr.sin_addr);
  ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
  if (ret < 0 && errno == EINPROGRESS)
    {
      /* NuttX poll's speculative cansend check logs while connecting.
       * Query connection state without emitting console output instead.
       */

      started = clock_systime_ticks();
      while (TICK2MSEC(clock_systime_ticks() - started) < 2000 &&
             !atomic_load(&g_voice.stop))
        {
          length = sizeof(addr);
          ret = getpeername(fd, (struct sockaddr *)&addr, &length);
          if (ret == 0)
            {
              break;
            }

          length = sizeof(error);
          if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0 ||
              error != 0)
            {
              break;
            }

          usleep(10000);
        }
    }

  if (ret < 0)
    {
      close(fd);
      return -ECONNREFUSED;
    }

  fcntl(fd, F_SETFL, 0);
  size = snprintf(header, sizeof(header),
    "POST /v1/asr/stream HTTP/1.1\r\nHost: %s:%d\r\n"
    "Content-Type: audio/pcm;rate=16000;channels=1;format=s16le\r\n"
    "Transfer-Encoding: chunked\r\nConnection: close\r\n"
#ifdef CONFIG_SYSTEM_TITAN_VOICE_INTENT
    "X-VoiceFE-Device: %s\r\nX-VoiceFE-Boot: %s\r\n"
    "X-VoiceFE-Utterance: %" PRIu64 "\r\n"
#endif
    "\r\n", g_voice.host, g_voice.port
#ifdef CONFIG_SYSTEM_TITAN_VOICE_INTENT
    , CONFIG_SYSTEM_TITAN_VOICE_INTENT_DEVICE, g_voice.boot,
    g_voice.net_utterance
#endif
    );
  if (voicefe_send(fd, header, size) < 0)
    {
      close(fd);
      return -EIO;
    }

  return fd;
}

static int voicefe_response(int fd, char *response, size_t capacity)
{
  struct timespec started;
  struct timespec now;
  struct pollfd pfd;
  char *header;
  char *line;
  char *end;
  unsigned long length = 0;
  size_t used = 0;
  size_t offset = 0;
  ssize_t received;
  bool found = false;
  bool complete = false;
  int ret;

  clock_gettime(CLOCK_MONOTONIC, &started);
  pfd.fd = fd;
  pfd.events = POLLIN;
  while (!atomic_load(&g_voice.stop))
    {
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (now.tv_sec - started.tv_sec >= 10)
        {
          return -ETIMEDOUT;
        }

      ret = poll(&pfd, 1, 100);
      if (ret < 0 && errno == EINTR)
        {
          continue;
        }

      if (ret < 0)
        {
          return -errno;
        }

      if (ret == 0)
        {
          continue;
        }

      received = recv(fd, response + used, capacity - 1 - used, 0);
      if (received < 0 && errno == EINTR)
        {
          continue;
        }

      if (received <= 0)
        {
          if (received == 0 && complete)
            {
              response[offset + length] = '\0';
              memmove(response, response + offset, length + 1);
              return 0;
            }

          return received == 0 ? -EPROTO : -errno;
        }

      used += received;
      response[used] = '\0';
      header = strstr(response, "\r\n\r\n");
      if (offset == 0 && header != NULL)
        {
          if (strncmp(response, "HTTP/1.1 200 ", 13) != 0)
            {
              return -EPROTO;
            }

          offset = header + 4 - response;
          line = strstr(response, "\r\n") + 2;
          while (line < header)
            {
              if (strncasecmp(line, "Content-Length:", 15) == 0)
                {
                  if (found)
                    {
                      return -EPROTO;
                    }

                  line += 15;
                  while (*line == ' ' || *line == '\t')
                    {
                      line++;
                    }

                  if (*line < '0' || *line > '9')
                    {
                      return -EPROTO;
                    }

                  errno = 0;
                  length = strtoul(line, &end, 10);
                  if (errno != 0 || strncmp(end, "\r\n", 2) != 0)
                    {
                      return -EPROTO;
                    }

                  found = true;
                }
              else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0)
                {
                  return -EPROTO;
                }

              line = strstr(line, "\r\n") + 2;
            }

          if (!found || length == 0 || length >= capacity - offset)
            {
              return -EMSGSIZE;
            }
        }

      if (offset != 0 && used >= offset + length)
        {
          if (used != offset + length)
            {
              return -EPROTO;
            }

          /* The service advertises Connection: close. Consume its FIN
           * before closing locally, so completed utterances do not retain
           * TCP callbacks in active-close TIME_WAIT connections.
           */

          complete = true;
        }

      if (used == capacity - 1)
        {
          return -EMSGSIZE;
        }
    }

  return -ECANCELED;
}

static void *voicefe_net(void *arg)
{
  int16_t chunk[VOICEFE_FRAMES * 2];
  char response[2048];
  unsigned int tail;
  unsigned int head;
  unsigned int state;
  unsigned int frames = 0;
  uint32_t started;
  int fd = -1;
  int ret;

  while (!atomic_load(&g_voice.stop))
    {
      state = atomic_load(&g_voice.session);
      if (state == VF_IDLE)
        {
          while (sem_wait(&g_voice.wake) < 0 && errno == EINTR)
            {
            }

          continue;
        }

      if (fd < 0 && state != VF_ABORT)
        {
          started = clock_systime_ticks();
          fd = voicefe_connect();
          atomic_store(&g_voice.connect_ms,
                       TICK2MSEC(clock_systime_ticks() - started));
          if (fd < 0)
            {
              pthread_mutex_lock(&g_result_lock);
              g_result_error = fd;
              g_result[0] = '\0';
              pthread_mutex_unlock(&g_result_lock);
              atomic_store(&g_voice.session, VF_ABORT);
              atomic_fetch_add(&g_voice.failures, 1);
            }
        }

      tail = atomic_load(&g_voice.tail);
      head = atomic_load(&g_voice.head);
      if (tail != head)
        {
          if (atomic_load(&g_voice.session) != VF_ABORT)
            {
              memcpy(chunk + frames, g_voice.queue[tail % VF_QUEUE],
                     VOICEFE_FRAMES * 2);
              frames += VOICEFE_FRAMES;
              if (frames == VOICEFE_FRAMES * 2)
                {
                  ret = voicefe_send(fd, "280\r\n", 5);
                  if (ret == 0)
                    {
                      ret = voicefe_send(fd, chunk, sizeof(chunk));
                    }

                  if (ret == 0)
                    {
                      ret = voicefe_send(fd, "\r\n", 2);
                    }

                  if (ret < 0)
                    {
                      atomic_store(&g_voice.session, VF_ABORT);
                      atomic_fetch_add(&g_voice.failures, 1);
                    }

                  frames = 0;
                }
            }

          atomic_store(&g_voice.tail, tail + 1);
          continue;
        }

      state = atomic_load(&g_voice.session);
      if (tail != atomic_load(&g_voice.head))
        {
          continue;
        }

      if (state == VF_LIVE ||
          (state == VF_ABORT && atomic_load(&g_voice.active)))
        {
          while (sem_wait(&g_voice.wake) < 0 && errno == EINTR)
            {
            }

          continue;
        }

      if (state == VF_END && fd >= 0)
        {
          ret = 0;
          if (frames != 0)
            {
              ret = voicefe_send(fd, "140\r\n", 5);
              if (ret == 0)
                {
                  ret = voicefe_send(fd, chunk, VOICEFE_FRAMES * 2);
                }

              if (ret == 0)
                {
                  ret = voicefe_send(fd, "\r\n", 2);
                }
            }

          if (ret == 0)
            {
              ret = voicefe_send(fd, "0\r\n\r\n", 5);
            }

          if (ret == 0)
            {
              ret = voicefe_response(fd, response, sizeof(response));
            }

          atomic_store(&g_voice.latency,
                       TICK2MSEC(clock_systime_ticks() -
                                 atomic_load(&g_voice.ended)));
          pthread_mutex_lock(&g_result_lock);
          g_result_error = ret;
          if (ret == 0 && strlen(response) < sizeof(g_result))
            {
              strcpy(g_result, response);
            }
          else
            {
              g_result[0] = '\0';
              if (ret == 0)
                {
                  g_result_error = -EMSGSIZE;
                }

              atomic_fetch_add(&g_voice.failures, 1);
            }

          pthread_mutex_unlock(&g_result_lock);
        }

      if (fd >= 0)
        {
          close(fd);
          fd = -1;
        }

      frames = 0;
      atomic_store(&g_voice.session, VF_IDLE);
    }

  if (fd >= 0)
    {
      close(fd);
    }

  return NULL;
}
#endif /* CONFIG_SYSTEM_TITAN_VOICE_ASR_HTTP */

static int voicefe_thread(pthread_t *thread, void *(*entry)(void *),
                          int priority, int stack)
{
  struct sched_param param;
  pthread_attr_t attr;
  int ret;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, stack);
  pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  param.sched_priority = priority;
  pthread_attr_setschedparam(&attr, &param);
  ret = pthread_create(thread, &attr, entry, NULL);
  pthread_attr_destroy(&attr);
  return ret;
}

static void voicefe_stats(void)
{
  struct pdm_stats_s pdm;

  pthread_mutex_lock(&g_pdm_lock);
  pdm = g_voice.pdm;
  pthread_mutex_unlock(&g_pdm_lock);

  printf("voicefe: %s blocks=%u utterances=%u failed=%u queue=%u/%u\n",
         g_voice.running && !atomic_load(&g_voice.stop) ?
         "running" : "stopped",
         atomic_load(&g_voice.blocks), atomic_load(&g_voice.utterances),
         atomic_load(&g_voice.failures), atomic_load(&g_voice.highwater),
         VF_QUEUE);
  printf("energy=%u noise=%u PDM frames=%" PRIu32
         " overruns=%" PRIu32 " desyncs=%" PRIu32
         " errors=%" PRIu32 " highwater=%" PRIu32 "\n",
         atomic_load(&g_voice.energy), atomic_load(&g_voice.noise),
         pdm.frames, pdm.overruns, pdm.desyncs, pdm.hwerrors, pdm.highwater);
  printf("PDM fifo=%" PRIu32 " level=%" PRIu32 " short=%" PRIu32 "\n",
         pdm.fifoerrors, pdm.levelerrors, pdm.shorterrors);
  pthread_mutex_lock(&g_result_lock);
  printf("ASR error=%d result=%s\n", g_result_error, g_result);
  pthread_mutex_unlock(&g_result_lock);
  printf("connect=%u ms end-to-response=%u ms\n",
         atomic_load(&g_voice.connect_ms), atomic_load(&g_voice.latency));
#ifdef CONFIG_SYSTEM_TITAN_VOICE_NS
  printf("NS=%s gain=%u/32768 clipped=%u latency=%u ms\n",
         g_voice.denoise ? "on" : "off", atomic_load(&g_voice.ns_gain),
         atomic_load(&g_voice.ns_clipped), g_voice.denoise ? 10 : 0);
#endif
#ifdef CONFIG_SYSTEM_TITAN_VOICE_PROFILE
  printf("DSP/enqueue cycles=%u avg=%u max=%u frequency=%lu\n",
         atomic_load(&g_voice.cycles), atomic_load(&g_voice.avgcycles),
         atomic_load(&g_voice.maxcycles),
         perf_getfreq());
#endif
}

static int voicefe_start(int argc, char **argv)
{
#if defined(CONFIG_SYSTEM_TITAN_VOICE_ASR_HTTP) || \
    defined(CONFIG_SYSTEM_TITAN_VOICE_INTENT_TELEMETRY)
  struct in_addr address;
#endif
  const char *host = CONFIG_SYSTEM_TITAN_VOICE_HOST;
  int port = CONFIG_SYSTEM_TITAN_VOICE_PORT;
  int mode = VOICEFE_LEFT;
#ifdef CONFIG_SYSTEM_TITAN_VOICE_NS
  bool denoise = true;
#endif
#ifdef CONFIG_SYSTEM_TITAN_VOICE_INTENT
  bool intent = true;
  struct timespec nonce;
#endif
  int ret;
  int i;

  if (g_voice.running)
    {
      return -EBUSY;
    }

  for (i = 2; i < argc; i += 2)
    {
      if (i + 1 == argc)
        {
          return -EINVAL;
        }

      if (strcmp(argv[i], "-h") == 0)
        {
          host = argv[i + 1];
        }
      else if (strcmp(argv[i], "-p") == 0)
        {
          port = atoi(argv[i + 1]);
        }
      else if (strcmp(argv[i], "-m") == 0)
        {
          mode = atoi(argv[i + 1]);
        }
      else if (strcmp(argv[i], "-n") == 0)
        {
          if (strcmp(argv[i + 1], "0") != 0 &&
              strcmp(argv[i + 1], "1") != 0)
            {
              return -EINVAL;
            }

#ifdef CONFIG_SYSTEM_TITAN_VOICE_NS
          denoise = strcmp(argv[i + 1], "1") == 0;
#else
          if (strcmp(argv[i + 1], "1") == 0)
            {
              return -ENOTSUP;
            }
#endif
        }
#ifdef CONFIG_SYSTEM_TITAN_VOICE_INTENT
      else if (strcmp(argv[i], "-i") == 0)
        {
          if (strcmp(argv[i + 1], "0") != 0 &&
              strcmp(argv[i + 1], "1") != 0)
            {
              return -EINVAL;
            }

          intent = strcmp(argv[i + 1], "1") == 0;
        }
#endif
      else
        {
          return -EINVAL;
        }
    }

  if (strlen(host) >= 16 || port < 1 || port > 65535 ||
      mode < 0 || mode > 3)
    {
      return -EINVAL;
    }

#if defined(CONFIG_SYSTEM_TITAN_VOICE_ASR_HTTP) || \
    defined(CONFIG_SYSTEM_TITAN_VOICE_INTENT_TELEMETRY)
  if (inet_pton(AF_INET, host, &address) != 1)
    {
      return -EINVAL;
    }
#endif

#ifndef CONFIG_SYSTEM_TITAN_VOICE_BF
  if (mode == VOICEFE_BEAM)
    {
      return -ENOTSUP;
    }
#endif

  memset(&g_voice, 0, sizeof(g_voice));
  pthread_mutex_lock(&g_result_lock);
  g_result[0] = '\0';
  g_result_error = 0;
  pthread_mutex_unlock(&g_result_lock);
  strcpy(g_voice.host, host);
  g_voice.port = port;
  voicefe_dsp_init(&g_voice.dsp, mode,
                  CONFIG_SYSTEM_TITAN_VOICE_LEFT_GAIN,
                  CONFIG_SYSTEM_TITAN_VOICE_RIGHT_GAIN);
#ifdef CONFIG_SYSTEM_TITAN_VOICE_NS
  g_voice.denoise = denoise;
  voicefe_ns_init(&g_voice.ns, CONFIG_AUDIOUTILS_TITAN_VOICEFE_NS_FLOOR);
  atomic_store(&g_voice.ns_gain, 32768);
#endif
  if (sem_init(&g_voice.wake, 0, 0) < 0)
    {
      return -errno;
    }

  sem_setprotocol(&g_voice.wake, SEM_PRIO_NONE);
  if (sem_init(&g_voice.begin, 0, 0) < 0)
    {
      ret = -errno;
      sem_destroy(&g_voice.wake);
      return ret;
    }

  sem_setprotocol(&g_voice.begin, SEM_PRIO_NONE);
#ifdef CONFIG_SYSTEM_TITAN_VOICE_INTENT
  g_voice.intent = intent;
  clock_gettime(CLOCK_REALTIME, &nonce);
  snprintf(g_voice.boot, sizeof(g_voice.boot), "%016" PRIx64,
           ((uint64_t)nonce.tv_sec << 32) ^ (uint32_t)nonce.tv_nsec ^
           clock_systime_ticks());
  if (intent)
    {
      ret = voicefe_intent_start();
      if (ret < 0)
        {
          sem_destroy(&g_voice.begin);
          sem_destroy(&g_voice.wake);
          return ret;
        }
    }
#endif

#ifdef CONFIG_SYSTEM_TITAN_VOICE_ASR_HTTP
  ret = voicefe_thread(&g_voice.net, voicefe_net,
                      CONFIG_SYSTEM_TITAN_VOICE_NET_PRIORITY,
                      CONFIG_SYSTEM_TITAN_VOICE_NET_STACKSIZE);
  if (ret != 0)
    {
      sem_destroy(&g_voice.begin);
      sem_destroy(&g_voice.wake);
#ifdef CONFIG_SYSTEM_TITAN_VOICE_INTENT
      if (intent)
        {
          voicefe_intent_stop();
        }
#endif

      return -ret;
    }
#endif

  ret = voicefe_thread(&g_voice.rt, voicefe_rt,
                      CONFIG_SYSTEM_TITAN_VOICE_PRIORITY,
                      CONFIG_SYSTEM_TITAN_VOICE_STACKSIZE);
  if (ret != 0)
    {
      atomic_store(&g_voice.stop, true);
      sem_post(&g_voice.wake);
#ifdef CONFIG_SYSTEM_TITAN_VOICE_ASR_HTTP
      pthread_join(g_voice.net, NULL);
#endif
#ifdef CONFIG_SYSTEM_TITAN_VOICE_INTENT
      if (intent)
        {
          voicefe_intent_stop();
        }
#endif

      sem_destroy(&g_voice.begin);
      sem_destroy(&g_voice.wake);
      return -ret;
    }

  pthread_setname_np(g_voice.rt, "voicefe_rt");
#ifdef CONFIG_SYSTEM_TITAN_VOICE_ASR_HTTP
  pthread_setname_np(g_voice.net, "voicefe_net");
#endif

  /* Allocate worker task groups and stacks before capture starts. */

  g_voice.fd = open("/dev/pdm0", O_RDONLY | O_NONBLOCK);
  if (g_voice.fd < 0)
    {
      ret = -errno;
      atomic_store(&g_voice.stop, true);
      sem_post(&g_voice.begin);
      sem_post(&g_voice.wake);
      pthread_join(g_voice.rt, NULL);
#ifdef CONFIG_SYSTEM_TITAN_VOICE_ASR_HTTP
      pthread_join(g_voice.net, NULL);
#endif
#ifdef CONFIG_SYSTEM_TITAN_VOICE_INTENT
      if (intent)
        {
          voicefe_intent_stop();
        }
#endif

      sem_destroy(&g_voice.begin);
      sem_destroy(&g_voice.wake);
      return ret;
    }

  sem_post(&g_voice.begin);
  g_voice.running = true;
  printf("voicefe: started mode=%d server=%s:%d\n", mode, host, port);
  return 0;
}

static int voicefe_daemon(int argc, char **argv)
{
  g_start_result = voicefe_start(argc, argv);
  sem_post(&g_ready);
  if (g_start_result < 0)
    {
      return EXIT_FAILURE;
    }

  /* Retain the task group: returning from its leader kills pthreads. */

#ifdef CONFIG_SYSTEM_TITAN_VOICE_INTENT_TELEMETRY
  voicefe_telemetry_init();
#endif
  while (!atomic_load(&g_voice.stop))
    {
      pthread_mutex_lock(&g_pdm_lock);
      ioctl(g_voice.fd, PDMIOC_GETSTATS, (unsigned long)&g_voice.pdm);
      pthread_mutex_unlock(&g_pdm_lock);
#ifdef CONFIG_SYSTEM_TITAN_VOICE_INTENT_TELEMETRY
      if (g_voice.intent)
        {
          voicefe_telemetry_tick(g_voice.host, g_voice.port, g_voice.boot);
        }
#endif

      usleep(100000);
    }

  pthread_join(g_voice.rt, NULL);
  atomic_store(&g_voice.stop, true);
  sem_post(&g_voice.wake);
#ifdef CONFIG_SYSTEM_TITAN_VOICE_ASR_HTTP
  pthread_join(g_voice.net, NULL);
#endif
#ifdef CONFIG_SYSTEM_TITAN_VOICE_INTENT
  if (g_voice.intent)
    {
      voicefe_intent_stop();
    }
#endif

  pthread_mutex_lock(&g_pdm_lock);
  ioctl(g_voice.fd, PDMIOC_GETSTATS, (unsigned long)&g_voice.pdm);
  pthread_mutex_unlock(&g_pdm_lock);
  close(g_voice.fd);
  sem_destroy(&g_voice.begin);
  sem_destroy(&g_voice.wake);
  sem_post(&g_done);
  return EXIT_SUCCESS;
}

#ifdef CONFIG_SYSTEM_TITAN_VOICE_DEBUG_CAPTURE
static int voicefe_capture(int argc, char **argv)
{
  uint64_t energy[2] =
  {
    0, 0
  };

  int64_t sum[2] =
  {
    0, 0
  };

  uint32_t peak[2] =
  {
    0, 0
  };

  uint32_t blocks;
  uint32_t i;
  uint32_t channel;
  int32_t value;
  int64_t lag = 0;
  int seconds = argc > 2 ? atoi(argv[2]) : 5;
  int fd;
  int ret = 0;

  if (g_voice.running || seconds < 1 || seconds > 86400)
    {
      return -EINVAL;
    }

  atomic_store(&g_voice.stop, false);
  fd = open("/dev/pdm0", O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    {
      return -errno;
    }

  for (blocks = 0; blocks < (uint32_t)seconds * 100; blocks++)
    {
      ret = voicefe_read(fd, g_voice.stereo);
      if (ret < 0)
        {
          break;
        }

      lag += voicefe_delay(g_voice.stereo, VOICEFE_FRAMES);
      for (i = 0; i < VOICEFE_FRAMES; i++)
        {
          for (channel = 0; channel < 2; channel++)
            {
              value = g_voice.stereo[2 * i + channel];
              sum[channel] += value;
              energy[channel] += (int64_t)value * value;
              value = value < 0 ? -value : value;
              if ((uint32_t)value > peak[channel])
                {
                  peak[channel] = value;
                }
            }
        }
    }

  ioctl(fd, PDMIOC_GETSTATS, (unsigned long)&g_voice.pdm);
  close(fd);
  if (blocks > 0)
    {
      for (channel = 0; channel < 2; channel++)
        {
          printf("channel=%" PRIu32 " RMS=%" PRIu32 " peak=%" PRIu32
                 " DC=%" PRId64 "\n", channel,
                 voicefe_isqrt(energy[channel] / (blocks * VOICEFE_FRAMES)),
                 peak[channel], sum[channel] / (blocks * VOICEFE_FRAMES));
        }

      printf("capture blocks=%" PRIu32 " mean delay=%" PRId64 "/1000\n",
             blocks, lag * 1000 / blocks);
    }

  voicefe_stats();
  return ret;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char **argv)
{
  int ret = 0;

  pthread_mutex_lock(&g_lock);
  if (argc < 2)
    {
      ret = -EINVAL;
    }
  else if (strcmp(argv[1], "start") == 0)
    {
      if (g_voice.running)
        {
          ret = -EBUSY;
        }
      else
        {
          sem_init(&g_ready, 0, 0);
          sem_init(&g_done, 0, 0);
          ret = task_create("voicefe", CONFIG_SYSTEM_TITAN_VOICE_PRIORITY,
                            4096, voicefe_daemon, &argv[1]);
          if (ret < 0)
            {
              ret = -errno;
            }
          else
            {
              while (sem_wait(&g_ready) < 0 && errno == EINTR)
                {
                }

              ret = g_start_result;
            }

          sem_destroy(&g_ready);
          if (ret < 0)
            {
              sem_destroy(&g_done);
            }
        }
    }
  else if (strcmp(argv[1], "stop") == 0)
    {
      if (g_voice.running)
        {
          atomic_store(&g_voice.stop, true);
          sem_post(&g_voice.wake);
          while (sem_wait(&g_done) < 0 && errno == EINTR)
            {
            }

          sem_destroy(&g_done);
          g_voice.running = false;
        }

      voicefe_stats();
    }
  else if (strcmp(argv[1], "status") == 0 ||
           strcmp(argv[1], "stats") == 0)
    {
      voicefe_stats();
    }
#ifdef CONFIG_SYSTEM_TITAN_VOICE_INTENT
  else if (strcmp(argv[1], "intent") == 0 && argc == 3)
    {
      if (!g_voice.running || !g_voice.intent)
        {
          ret = -ENODEV;
        }
      else if (strcmp(argv[2], "reset") == 0)
        {
          voicefe_intent_reset();
        }
      else if (strcmp(argv[2], "status") == 0 ||
               strcmp(argv[2], "events") == 0)
        {
          voicefe_intent_print(strcmp(argv[2], "events") == 0);
        }
      else
        {
          ret = -EINVAL;
        }
    }
#endif
#ifdef CONFIG_SYSTEM_TITAN_VOICE_DEBUG_CAPTURE
  else if (strcmp(argv[1], "capture") == 0)
    {
      ret = voicefe_capture(argc, argv);
    }
#endif
  else
    {
      ret = -EINVAL;
    }

  pthread_mutex_unlock(&g_lock);
  if (ret < 0)
    {
      fprintf(stderr, "voicefe: error %d\n"
              "usage: voicefe start [-h IPv4] [-p port] [-m 0..3]\n"
              "                     [-n 0|1]"
#ifdef CONFIG_SYSTEM_TITAN_VOICE_INTENT
              " [-i 0|1]"
#endif
              "\n"
              "       voicefe stop|status|stats|capture [seconds]\n"
#ifdef CONFIG_SYSTEM_TITAN_VOICE_INTENT
              "       voicefe intent status|events|reset\n"
#endif
              , ret);
      return EXIT_FAILURE;
    }

  return EXIT_SUCCESS;
}
