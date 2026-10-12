/****************************************************************************
 * apps/mlearning/tflite-micro/tflm_titan_intent.cc
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <new>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/aie/ethosu.h>
#include <audioutils/titan_intent.h>
#include <audioutils/titan_voicefe.h>

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "models/titan_intent/metadata.h"

extern "C"
{
  extern const unsigned char g_titan_wake_model[];
  extern const unsigned char g_titan_command_model[];
}

namespace
{
  using Interpreter = tflite::MicroInterpreter;

  alignas(32) uint8_t g_wake_arena[
    CONFIG_TFLITEMICRO_TITAN_INTENT_WAKE_ARENA];
  alignas(32) uint8_t g_command_arena[
    CONFIG_TFLITEMICRO_TITAN_INTENT_COMMAND_ARENA];
  alignas(Interpreter) uint8_t g_storage[2][sizeof(Interpreter)];
  tflite::MicroMutableOpResolver<2> g_resolver;
  Interpreter *g_interpreters[2];
  bool g_resolver_ready;
  int g_device = -1;

  int initialize_model(unsigned int index, const unsigned char *content,
                       uint8_t *arena, size_t capacity, unsigned int rows,
                       unsigned int classes, float input_scale,
                       int input_zero, float output_scale, int output_zero)
  {
    const tflite::Model *model = tflite::GetModel(content);
    if (model->version() != TFLITE_SCHEMA_VERSION)
      {
        return -EPROTO;
      }

    Interpreter *interpreter = new (g_storage[index]) Interpreter(
      model, g_resolver, arena, capacity);
    g_interpreters[index] = interpreter;
    if (interpreter->AllocateTensors() != kTfLiteOk)
      {
        return -ENOMEM;
      }

    TfLiteTensor *input = interpreter->input(0);
    TfLiteTensor *output = interpreter->output(0);
    if (input == nullptr || output == nullptr ||
        input->type != kTfLiteInt8 || output->type != kTfLiteInt8 ||
        input->dims->size != 4 || input->dims->data[0] != 1 ||
        input->dims->data[1] != static_cast<int>(rows) ||
        input->dims->data[2] != VOICEFE_MELS ||
        input->dims->data[3] != 1 ||
        input->bytes != rows * VOICEFE_MELS || output->bytes != classes ||
        input->params.zero_point != input_zero ||
        output->params.zero_point != output_zero ||
        fabsf(input->params.scale - input_scale) > 1e-6f ||
        fabsf(output->params.scale - output_scale) > 1e-6f ||
        (reinterpret_cast<uintptr_t>(input->data.int8) & 15) != 0)
      {
        return -EPROTO;
      }

    return 0;
  }
}

extern "C" int tflm_titan_intent_init(void)
{
  struct ethosu_info_s info = {};
  int ret;

  if (g_device >= 0)
    {
      return -EBUSY;
    }

  g_device = open(CONFIG_TFLITEMICRO_ETHOSU_DEVPATH, O_RDWR);
  if (g_device < 0)
    {
      return -errno;
    }

  ret = ioctl(g_device, ETHOSUIOC_GET_INFO,
              reinterpret_cast<unsigned long>(&info));
  if (ret < 0)
    {
      ret = -errno;
      tflm_titan_intent_deinit();
      return ret;
    }

  if (!g_resolver_ready)
    {
      if (g_resolver.AddEthosU() != kTfLiteOk ||
          g_resolver.AddReshape() != kTfLiteOk)
        {
          tflm_titan_intent_deinit();
          return -EINVAL;
        }

      g_resolver_ready = true;
    }

  ret = initialize_model(0, g_titan_wake_model, g_wake_arena,
                        sizeof(g_wake_arena), VOICEFE_KWS_ROWS,
                        TITAN_WAKE_CLASSES, TITAN_MODEL_WAKE_INPUT_SCALE,
                        TITAN_MODEL_WAKE_INPUT_ZERO,
                        TITAN_MODEL_WAKE_OUTPUT_SCALE,
                        TITAN_MODEL_WAKE_OUTPUT_ZERO);
  if (ret == 0)
    {
      ret = initialize_model(1, g_titan_command_model, g_command_arena,
                            sizeof(g_command_arena), VOICEFE_COMMAND_ROWS,
                            TITAN_INTENT_CLASSES,
                            TITAN_MODEL_COMMAND_INPUT_SCALE,
                            TITAN_MODEL_COMMAND_INPUT_ZERO,
                            TITAN_MODEL_COMMAND_OUTPUT_SCALE,
                            TITAN_MODEL_COMMAND_OUTPUT_ZERO);
    }

  if (ret < 0)
    {
      tflm_titan_intent_deinit();
    }

  return ret;
}

extern "C" void tflm_titan_intent_deinit(void)
{
  for (unsigned int i = 0; i < 2; i++)
    {
      if (g_interpreters[i] != nullptr)
        {
          g_interpreters[i]->~Interpreter();
          g_interpreters[i] = nullptr;
        }
    }

  if (g_device >= 0)
    {
      close(g_device);
      g_device = -1;
    }
}

extern "C" int tflm_titan_intent_invoke(
  bool wake, const int8_t *features, size_t bytes,
  struct titan_intent_score_s *score, uint64_t *cycles, uint32_t *elapsed_us)
{
  Interpreter *interpreter = g_interpreters[wake ? 0 : 1];
  const int8_t *table = wake ? g_titan_wake_quant : g_titan_command_quant;
  struct ethosu_stats_s stats = {};
  struct timespec start;
  struct timespec end;
  int ret;

  if (interpreter == nullptr || g_device < 0)
    {
      return -ENODEV;
    }

  TfLiteTensor *input = interpreter->input(0);
  TfLiteTensor *output = interpreter->output(0);
  if (bytes != input->bytes)
    {
      return -EINVAL;
    }

  for (size_t i = 0; i < bytes; i++)
    {
      input->data.int8[i] = table[static_cast<int>(features[i]) + 128];
    }

  clock_gettime(CLOCK_MONOTONIC, &start);
  if (interpreter->Invoke() != kTfLiteOk)
    {
      return -EIO;
    }

  clock_gettime(CLOCK_MONOTONIC, &end);
  ret = ioctl(g_device, ETHOSUIOC_GET_STATS,
              reinterpret_cast<unsigned long>(&stats));
  if (ret < 0)
    {
      return -errno;
    }

  *cycles = stats.pmu_cycles;
  *elapsed_us = (end.tv_sec - start.tv_sec) * 1000000 +
               (end.tv_nsec - start.tv_nsec) / 1000;
  titan_intent_score(output->data.int8, output->bytes,
                     wake ? TITAN_MODEL_WAKE_SCALE_Q20 :
                     TITAN_MODEL_COMMAND_SCALE_Q20, score);
  return 0;
}

extern "C" size_t tflm_titan_intent_arena(bool wake)
{
  Interpreter *interpreter = g_interpreters[wake ? 0 : 1];
  return interpreter == nullptr ? 0 : interpreter->arena_used_bytes();
}

extern "C" const char *tflm_titan_intent_version(void)
{
  return TITAN_MODEL_VERSION;
}
