/****************************************************************************
 * apps/mlearning/tflite-micro/ethosu_vww_main.cc
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/aie/ethosu.h>

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

extern "C"
{
  extern const unsigned char g_ethosu_vww_model[];
  extern const unsigned char g_ethosu_vww_model_end[];
}

static uint64_t elapsed_us(const struct timespec &start,
                           const struct timespec &end)
{
  uint64_t seconds = static_cast<uint64_t>(end.tv_sec - start.tv_sec);
  int64_t nanoseconds = end.tv_nsec - start.tv_nsec;

  if (nanoseconds < 0)
    {
      seconds--;
      nanoseconds += 1000000000;
    }

  return seconds * 1000000 + static_cast<uint64_t>(nanoseconds / 1000);
}

static int run_model(uint8_t *arena)
{
  tflite::MicroMutableOpResolver<4> resolver;
  const tflite::Model *model;
  tflite::MicroInterpreter *interpreter;
  struct ethosu_stats_s stats = {};
  struct timespec start;
  struct timespec end;
  TfLiteTensor *input;
  TfLiteTensor *output;
  int fd;

  model = tflite::GetModel(g_ethosu_vww_model);
  if (model->version() != TFLITE_SCHEMA_VERSION)
    {
      fprintf(stderr, "tflm_ethosu_vww: schema %lu is unsupported\n",
              static_cast<unsigned long>(model->version()));
      return 1;
    }

  if (resolver.AddEthosU() != kTfLiteOk ||
      resolver.AddPack() != kTfLiteOk ||
      resolver.AddReshape() != kTfLiteOk ||
      resolver.AddStridedSlice() != kTfLiteOk)
    {
      fprintf(stderr, "tflm_ethosu_vww: resolver setup failed\n");
      return 1;
    }

  tflite::MicroInterpreter local_interpreter(
    model, resolver, arena, CONFIG_TFLITEMICRO_ETHOSU_VWW_ARENA_SIZE);
  interpreter = &local_interpreter;

  if (interpreter->AllocateTensors() != kTfLiteOk)
    {
      fprintf(stderr,
              "tflm_ethosu_vww: arena allocation failed (%u bytes)\n",
              CONFIG_TFLITEMICRO_ETHOSU_VWW_ARENA_SIZE);
      return 1;
    }

  input = interpreter->input(0);
  output = interpreter->output(0);
  if (input == nullptr || output == nullptr || input->type != kTfLiteInt8 ||
      output->type != kTfLiteInt8 || input->bytes == 0 || output->bytes < 2)
    {
      fprintf(stderr, "tflm_ethosu_vww: unexpected model IO\n");
      return 1;
    }

  memset(input->data.int8, input->params.zero_point, input->bytes);
  clock_gettime(CLOCK_MONOTONIC, &start);
  if (interpreter->Invoke() != kTfLiteOk)
    {
      fprintf(stderr, "tflm_ethosu_vww: inference failed\n");
      return 1;
    }

  clock_gettime(CLOCK_MONOTONIC, &end);
  fd = open(CONFIG_TFLITEMICRO_ETHOSU_DEVPATH, O_RDONLY);
  if (fd < 0)
    {
      fprintf(stderr, "tflm_ethosu_vww: open stats failed: %d\n", errno);
      return 1;
    }

  if (ioctl(fd, ETHOSUIOC_GET_STATS,
            (unsigned long)(uintptr_t)&stats) < 0)
    {
      fprintf(stderr, "tflm_ethosu_vww: get stats failed: %d\n", errno);
      close(fd);
      return 1;
    }

  close(fd);
  printf("model: vww96_u55_256 (%lu bytes)\n",
         static_cast<unsigned long>(g_ethosu_vww_model_end -
                                    g_ethosu_vww_model));
  printf("arena: %lu/%u bytes\n",
         static_cast<unsigned long>(interpreter->arena_used_bytes()),
         CONFIG_TFLITEMICRO_ETHOSU_VWW_ARENA_SIZE);
  printf("output: not_person=%d person=%d\n",
         static_cast<int>(output->data.int8[0]),
         static_cast<int>(output->data.int8[1]));
  printf("elapsed: %llu us\n",
         static_cast<unsigned long long>(elapsed_us(start, end)));
  printf("npu: %llu cycles, result=%" PRId32 "\n",
         static_cast<unsigned long long>(stats.pmu_cycles), stats.result);
  return 0;
}

extern "C" int main(int argc, char **argv)
{
  void *arena;
  int ret;

  if (posix_memalign(&arena, 32,
                    CONFIG_TFLITEMICRO_ETHOSU_VWW_ARENA_SIZE) != 0)
    {
      fprintf(stderr, "tflm_ethosu_vww: no memory for tensor arena\n");
      return 1;
    }

  ret = run_model(static_cast<uint8_t *>(arena));
  free(arena);
  return ret;
}
