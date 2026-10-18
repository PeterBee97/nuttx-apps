#include <nuttx/config.h>
#include <errno.h>
#include <fcntl.h>
#include <new>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <nuttx/clock.h>
#include <nuttx/aie/ethosu.h>
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "board_metadata.h"

extern "C" {
extern const unsigned char board_model0[];
extern const unsigned char board_model1[];
}

namespace {
using Interpreter = tflite::MicroInterpreter;
alignas(32) uint8_t arenas0[32768];
alignas(32) uint8_t arenas1[65536];
alignas(Interpreter) uint8_t storage[2][sizeof(Interpreter)];
tflite::MicroMutableOpResolver<2> resolver;
Interpreter *models[2];
bool ready;
int device = -1;
}

extern "C" void board_model_close(void)
{
  for (unsigned i = 0; i < 2; i++)
    {
      if (models[i]) { models[i]->~Interpreter(); models[i] = nullptr; }
    }
  if (device >= 0) { close(device); device = -1; }
}

extern "C" int board_model_init(void)
{
  if (device >= 0) return -EBUSY;
  device = open(CONFIG_TFLITEMICRO_ETHOSU_DEVPATH, O_RDWR);
  if (device < 0) return -errno;
  if (!ready)
    {
      if (resolver.AddEthosU() != kTfLiteOk || resolver.AddReshape() != kTfLiteOk)
        { board_model_close(); return -EINVAL; }
      ready = true;
    }
  const unsigned char *bytes[] = {board_model0, board_model1};
  uint8_t *arenas[] = {arenas0, arenas1};
  const size_t capacities[] = {sizeof(arenas0), sizeof(arenas1)};
  const size_t inputs[] = {3960, 7960};
  const size_t outputs[] = {4, 30300};
  const float scales[] = {MODEL0_INPUT_SCALE, MODEL1_INPUT_SCALE};
  const float outscales[] = {MODEL0_OUTPUT_SCALE, MODEL1_OUTPUT_SCALE};
  const int zeros[] = {MODEL0_INPUT_ZERO, MODEL1_INPUT_ZERO};
  const int outzeros[] = {MODEL0_OUTPUT_ZERO, MODEL1_OUTPUT_ZERO};
  for (unsigned i = 0; i < 2; i++)
    {
      auto model = tflite::GetModel(bytes[i]);
      if (model->version() != TFLITE_SCHEMA_VERSION)
        { board_model_close(); return -EPROTO; }
      models[i] = new (storage[i]) Interpreter(model, resolver, arenas[i], capacities[i]);
      if (models[i]->AllocateTensors() != kTfLiteOk)
        { board_model_close(); return -ENOMEM; }
      auto in = models[i]->input(0);
      auto out = models[i]->output(0);
      if (!in || !out || in->type != kTfLiteInt8 || out->type != kTfLiteInt8 ||
          in->bytes != inputs[i] || out->bytes != outputs[i] ||
          in->params.zero_point != zeros[i] || out->params.zero_point != outzeros[i] ||
          fabsf(in->params.scale - scales[i]) > 1e-6f ||
          fabsf(out->params.scale - outscales[i]) > 1e-6f ||
          (reinterpret_cast<uintptr_t>(in->data.int8) & 15))
        { board_model_close(); return -EPROTO; }
    }
  return 0;
}

extern "C" size_t board_model_arena(bool wake)
{
  auto model = models[wake ? 0 : 1];
  return model ? model->arena_used_bytes() : 0;
}

extern "C" int board_model_run(bool wake, const int8_t *features,
                               size_t bytes, int8_t *output, size_t capacity,
                               uint64_t *cycles, uint32_t *micros)
{
  auto model = models[wake ? 0 : 1];
  if (!model || device < 0) return -ENODEV;
  auto in = model->input(0);
  auto out = model->output(0);
  if (bytes != in->bytes || capacity != out->bytes) return -EINVAL;
  const int8_t *table = wake ? quant0 : quant1;
  for (size_t i = 0; i < bytes; i++) in->data.int8[i] = table[(int)features[i] + 128];
  uint32_t start = perf_gettime();
  if (model->Invoke() != kTfLiteOk) return -EIO;
  uint32_t elapsed = (uint32_t)perf_gettime() - start;
  *micros = ((uint64_t)elapsed * 1000000 + perf_getfreq() - 1) / perf_getfreq();
  ethosu_stats_s stats = {};
  if (ioctl(device, ETHOSUIOC_GET_STATS, reinterpret_cast<unsigned long>(&stats)) < 0)
    return -errno;
  *cycles = stats.pmu_cycles;
  memcpy(output, out->data.int8, capacity);
  return 0;
}
