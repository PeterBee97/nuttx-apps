/****************************************************************************
 * apps/testing/titan_intent/host_arena.cc
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <fstream>
#include <iterator>
#include <vector>

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_context.h"
#include "tensorflow/lite/micro/kernels/kernel_util.h"

/* Allocation-only replacement for the device op. The persistent int matches
 * tflm_ethosu.cc. This cannot run inference: Invoke always returns an error.
 */

static void *host_init(TfLiteContext *context, const char *data, size_t length)
{
  return context->AllocatePersistentBuffer(context, sizeof(int));
}

static TfLiteStatus host_prepare(TfLiteContext *context, TfLiteNode *node)
{
  tflite::MicroContext *micro = tflite::GetMicroContext(context);
  TfLiteTensor *tensor = micro->AllocateTempInputTensor(node, 0);

  if (tensor == nullptr)
    {
      return kTfLiteError;
    }

  *static_cast<int *>(node->user_data) = tensor->bytes;
  micro->DeallocateTempTfLiteTensor(tensor);
  return kTfLiteOk;
}

static TfLiteStatus host_invoke(TfLiteContext *context, TfLiteNode *node)
{
  return kTfLiteError;
}

int main(int argc, char **argv)
{
  if (argc != 3)
    {
      fprintf(stderr, "Usage: host_arena model_vela.tflite arena_bytes\n");
      return 2;
    }

  std::ifstream file(argv[1], std::ios::binary);
  std::vector<char> data((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
  size_t bytes = strtoul(argv[2], nullptr, 10);
  void *arena;
  if (data.empty() || bytes == 0 || posix_memalign(&arena, 32, bytes) != 0)
    {
      return 2;
    }

  tflite::MicroMutableOpResolver<1> resolver;
  TFLMRegistration registration =
    tflite::micro::RegisterOp(host_init, host_prepare, host_invoke);
  resolver.AddCustom("ethos-u", &registration);
  tflite::MicroInterpreter interpreter(
    tflite::GetModel(data.data()), resolver,
    static_cast<uint8_t *>(arena), bytes);
  TfLiteStatus ret = interpreter.AllocateTensors();
  if (ret == kTfLiteOk)
    {
      TfLiteTensor *input = interpreter.input(0);
      TfLiteTensor *output = interpreter.output(0);
      printf("allocated: %zu/%zu bytes, input=%zu, output=%zu\n",
             interpreter.arena_used_bytes(), bytes,
             input->bytes, output->bytes);
    }
  else
    {
      fprintf(stderr, "allocation failed: %zu bytes\n", bytes);
    }

  /* Interpreter owns no heap resources here. Keep arena alive until its
   * destructor runs at process exit.
   */

  return ret == kTfLiteOk ? 0 : 1;
}
