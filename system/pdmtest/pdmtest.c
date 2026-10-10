/****************************************************************************
 * apps/system/pdmtest/pdmtest.c
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

#include <sys/types.h>
#include <poll.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define PDMTEST_DEVICE_DEFAULT       "/dev/pdm0"
#define PDMTEST_SAMPLE_RATE          16000
#define PDMTEST_CHANNELS             2
#define PDMTEST_CHUNK_FRAMES         320
#define PDMTEST_READ_TIMEOUT_MS      2000
#define PDMTEST_MIN_SECONDS          1
#define PDMTEST_MAX_SECONDS          60

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct pdmtest_stats_s
{
  uint64_t frames;
  uint64_t energy[2];
  uint64_t absolute[2];
  uint64_t difference;
  uint64_t identical;
  int64_t cross;
  int16_t minimum[2];
  int16_t maximum[2];
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint64_t pdmtest_isqrt(uint64_t value)
{
  uint64_t bit = UINT64_C(1) << 62;
  uint64_t root = 0;

  while (bit > value)
    {
      bit >>= 2;
    }

  while (bit != 0)
    {
      if (value >= root + bit)
        {
          value -= root + bit;
          root = (root >> 1) + bit;
        }
      else
        {
          root >>= 1;
        }

      bit >>= 2;
    }

  return root;
}

static uint32_t pdmtest_abs16(int16_t value)
{
  return value < 0 ? (uint32_t)(-(int32_t)value) : (uint32_t)value;
}

static void pdmtest_accumulate(FAR struct pdmtest_stats_s *stats,
                               FAR const int16_t *samples, size_t frames)
{
  size_t i;

  for (i = 0; i < frames; i++)
    {
      int32_t left = samples[i * PDMTEST_CHANNELS];
      int32_t right = samples[i * PDMTEST_CHANNELS + 1];
      int32_t difference = left - right;

      if (left < stats->minimum[0])
        {
          stats->minimum[0] = left;
        }

      if (left > stats->maximum[0])
        {
          stats->maximum[0] = left;
        }

      if (right < stats->minimum[1])
        {
          stats->minimum[1] = right;
        }

      if (right > stats->maximum[1])
        {
          stats->maximum[1] = right;
        }

      stats->energy[0] += (uint64_t)((int64_t)left * left);
      stats->energy[1] += (uint64_t)((int64_t)right * right);
      stats->absolute[0] += pdmtest_abs16(left);
      stats->absolute[1] += pdmtest_abs16(right);
      stats->difference += difference < 0 ? -difference : difference;
      stats->cross += (int64_t)left * right;
      if (left == right)
        {
          stats->identical++;
        }
    }

  stats->frames += frames;
}

static void pdmtest_usage(FAR const char *progname)
{
  fprintf(stderr, "Usage: %s [-d device] [-t seconds]\n", progname);
}

static int pdmtest_report(FAR const struct pdmtest_stats_s *stats)
{
  uint64_t meansquare[2];
  uint64_t rms[2];
  uint64_t denominator;
  uint64_t identical;
  uint32_t peak[2];
  int64_t correlation;
  bool pass;

  if (stats->frames == 0)
    {
      fprintf(stderr, "pdmtest: no stereo frames captured\n");
      return EXIT_FAILURE;
    }

  meansquare[0] = stats->energy[0] / stats->frames;
  meansquare[1] = stats->energy[1] / stats->frames;
  rms[0] = pdmtest_isqrt(meansquare[0]);
  rms[1] = pdmtest_isqrt(meansquare[1]);
  denominator = pdmtest_isqrt(meansquare[0] * meansquare[1]);
  correlation = denominator == 0 ? 0 :
                (stats->cross / (int64_t)stats->frames) * 1000 /
                (int64_t)denominator;
  identical = stats->identical * 1000 / stats->frames;

  peak[0] = pdmtest_abs16(stats->minimum[0]);
  if (peak[0] < pdmtest_abs16(stats->maximum[0]))
    {
      peak[0] = pdmtest_abs16(stats->maximum[0]);
    }

  peak[1] = pdmtest_abs16(stats->minimum[1]);
  if (peak[1] < pdmtest_abs16(stats->maximum[1]))
    {
      peak[1] = pdmtest_abs16(stats->maximum[1]);
    }

  printf("format: 16000 Hz, S16_LE, stereo interleaved\n");
  printf("mapping: left=U14/PDM2-falling, right=U13/PDM2-rising\n");
  printf("frames: %" PRIu64 "\n", stats->frames);
  printf("left : min=%d max=%d peak=%" PRIu32 " rms=%" PRIu64
         " mean_abs=%" PRIu64 "\n",
         stats->minimum[0], stats->maximum[0], peak[0], rms[0],
         stats->absolute[0] / stats->frames);
  printf("right: min=%d max=%d peak=%" PRIu32 " rms=%" PRIu64
         " mean_abs=%" PRIu64 "\n",
         stats->minimum[1], stats->maximum[1], peak[1], rms[1],
         stats->absolute[1] / stats->frames);
  printf("stereo: mean_abs_diff=%" PRIu64
         " identical=%" PRIu64 ".%" PRIu64 "%% corr=%" PRId64 ".%03"
         PRId64 "\n",
         stats->difference / stats->frames,
         identical / 10, identical % 10,
         correlation / 1000, llabs(correlation % 1000));

  pass = peak[0] > 8 && peak[1] > 8 && identical < 999;
  printf("result: %s\n", pass ? "PASS" : "FAIL");
  if (!pass)
    {
      printf("Speak near each microphone and retry; copied or silent "
             "channels must fail.\n");
    }

  return pass ? EXIT_SUCCESS : EXIT_FAILURE;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct pdmtest_stats_s stats =
  {
    .minimum =
    {
      INT16_MAX, INT16_MAX
    },
    .maximum =
    {
      INT16_MIN, INT16_MIN
    }
  };

  struct pollfd pfd;
  int16_t samples[PDMTEST_CHUNK_FRAMES * PDMTEST_CHANNELS];
  FAR const char *device = PDMTEST_DEVICE_DEFAULT;
  uint64_t target;
  long seconds = 2;
  int option;
  int fd;

  optind = 1;
  while ((option = getopt(argc, argv, "d:t:h")) != ERROR)
    {
      switch (option)
        {
          case 'd':
            device = optarg;
            break;

          case 't':
            seconds = strtol(optarg, NULL, 10);
            break;

          case 'h':
          default:
            pdmtest_usage(argv[0]);
            return option == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

  if (seconds < PDMTEST_MIN_SECONDS || seconds > PDMTEST_MAX_SECONDS)
    {
      pdmtest_usage(argv[0]);
      return EXIT_FAILURE;
    }

  fd = open(device, O_RDONLY);
  if (fd < 0)
    {
      fprintf(stderr, "pdmtest: open %s failed: %d\n", device, errno);
      return EXIT_FAILURE;
    }

  pfd.fd = fd;
  pfd.events = POLLIN;
  target = (uint64_t)seconds * PDMTEST_SAMPLE_RATE;
  while (stats.frames < target)
    {
      size_t remaining = target - stats.frames;
      size_t frames = remaining < PDMTEST_CHUNK_FRAMES ?
                      remaining : PDMTEST_CHUNK_FRAMES;
      ssize_t nbytes;
      int ret;

      pfd.revents = 0;
      ret = poll(&pfd, 1, PDMTEST_READ_TIMEOUT_MS);
      if (ret <= 0)
        {
          fprintf(stderr, "pdmtest: capture timeout or poll error: %d\n",
                  ret < 0 ? errno : ETIMEDOUT);
          close(fd);
          return EXIT_FAILURE;
        }

      nbytes = read(fd, samples,
                    frames * PDMTEST_CHANNELS * sizeof(int16_t));
      if (nbytes < 0)
        {
          fprintf(stderr, "pdmtest: read failed: %d\n", errno);
          close(fd);
          return EXIT_FAILURE;
        }

      if ((nbytes % (PDMTEST_CHANNELS * sizeof(int16_t))) != 0)
        {
          fprintf(stderr, "pdmtest: unaligned stereo read: %zd\n", nbytes);
          close(fd);
          return EXIT_FAILURE;
        }

      pdmtest_accumulate(&stats, samples,
                         nbytes / (PDMTEST_CHANNELS * sizeof(int16_t)));
    }

  close(fd);
  return pdmtest_report(&stats);
}
