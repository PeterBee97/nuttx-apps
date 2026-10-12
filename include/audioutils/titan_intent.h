/****************************************************************************
 * apps/include/audioutils/titan_intent.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APPS_INCLUDE_AUDIOUTILS_TITAN_INTENT_H
#define __APPS_INCLUDE_AUDIOUTILS_TITAN_INTENT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TITAN_INTENT_VERSION       1
#define TITAN_INTENT_ACTIONS       8
#define TITAN_INTENT_CLASSES       10
#define TITAN_WAKE_CLASSES         4
#define TITAN_INTENT_MODES         3
#define TITAN_INTENT_PCM_SAMPLES   160

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum titan_intent_e
{
  TITAN_PHOTO_TAKE,
  TITAN_VIDEO_START,
  TITAN_VIDEO_STOP,
  TITAN_AUDIO_START,
  TITAN_AUDIO_STOP,
  TITAN_COMPANION_ENTER,
  TITAN_COMPANION_EXIT,
  TITAN_STOP_RECENT,
  TITAN_UNKNOWN,
  TITAN_SILENCE
};

enum titan_wake_e
{
  TITAN_WAKE_INLINE,
  TITAN_WAKE_WINDOW,
  TITAN_WAKE_UNKNOWN,
  TITAN_WAKE_SILENCE
};

enum titan_mode_e
{
  TITAN_MODE_NONE = -1,
  TITAN_MODE_VIDEO,
  TITAN_MODE_AUDIO,
  TITAN_MODE_COMPANION
};

enum titan_event_e
{
  TITAN_EVENT_INTENT,
  TITAN_EVENT_WAKE_INLINE,
  TITAN_EVENT_WAKE_WINDOW,
  TITAN_EVENT_RESET
};

enum titan_result_e
{
  TITAN_RESULT_APPLIED,
  TITAN_RESULT_NOOP,
  TITAN_RESULT_COOLDOWN
};

struct titan_intent_score_s
{
  uint16_t score;
  uint16_t margin;
  uint8_t label;
};

struct titan_intent_event_s
{
  uint64_t seq;
  uint64_t utterance_id;
  uint64_t monotonic_ms;
  uint32_t generation;
  uint16_t score;
  uint16_t margin;
  uint8_t version;
  uint8_t kind;
  uint8_t intent;
  int8_t resolved;
  uint8_t result;
  bool active[TITAN_INTENT_MODES];
};

struct titan_intent_state_s
{
  uint64_t seq;
  uint64_t order;
  uint64_t started[TITAN_INTENT_MODES];
  uint64_t wake_deadline;
  uint32_t generation;
  uint32_t unknown;
  uint32_t denied;
  uint32_t too_long;
  uint32_t cooldown;
  bool active[TITAN_INTENT_MODES];
};

struct titan_intent_policy_s
{
  uint32_t window_ms;
  uint32_t cooldown_ms;
  uint16_t wake_score;
  uint16_t wake_margin;
  uint16_t command_score;
  uint16_t command_margin;
};

/* One ML worker owns this context. Snapshot readers must synchronize outside
 * the pure state machine. Acoustic discontinuities cancel the current turn
 * without changing the three mode states.
 */

struct titan_intent_s
{
  struct titan_intent_state_s state;
  struct titan_intent_policy_s policy;
  struct titan_intent_score_s wake;
  uint64_t current;
  uint64_t last_turn;
  uint64_t last_action[TITAN_INTENT_ACTIONS];
  uint16_t action_valid;
  uint8_t wake_history;
  uint8_t window_history;
  bool in_turn;
  bool authorized;
  bool emitted;
  bool inline_seen;
  bool window_seen;
  bool invalid;
};

struct titan_intent_block_s
{
  uint64_t utterance_id;
  uint64_t start_sample;
  uint32_t sequence;
  uint32_t generation;
  int16_t pcm[TITAN_INTENT_PCM_SAMPLES];
  bool speech;
  bool marker_end;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/* Scores use Q15 posterior probabilities. Logit scale is in Q20; the common
 * zero point cancels in softmax. This function does not use floating point.
 */

void titan_intent_score(const int8_t *logits, unsigned int classes,
                        uint32_t scale_q20,
                        struct titan_intent_score_s *result);
bool titan_intent_score_accept(const struct titan_intent_score_s *score,
                               unsigned int actions, uint16_t threshold,
                               uint16_t margin);
void titan_intent_init(struct titan_intent_s *ctx,
                       const struct titan_intent_policy_s *policy,
                       uint32_t generation);
bool titan_intent_begin(struct titan_intent_s *ctx, uint64_t utterance,
                        uint64_t now, uint32_t generation);
void titan_intent_wake(struct titan_intent_s *ctx,
                       const struct titan_intent_score_s *score);
bool titan_intent_candidate(struct titan_intent_s *ctx, uint64_t utterance,
                            uint32_t generation, uint64_t now,
                            const struct titan_intent_score_s *score,
                            bool too_long,
                            struct titan_intent_event_s *event);
bool titan_intent_end(struct titan_intent_s *ctx, uint64_t utterance,
                      uint64_t now, struct titan_intent_event_s *event);
void titan_intent_cancel(struct titan_intent_s *ctx);
void titan_intent_reset(struct titan_intent_s *ctx, uint64_t now,
                        struct titan_intent_event_s *event);
const char *titan_intent_name(unsigned int intent);
const char *titan_intent_event_name(unsigned int kind);
const char *titan_intent_result_name(unsigned int result);

/* The two interpreters are initialized before PDM capture and used only by
 * the ML worker. Inputs use the common C feature units, not tensor units.
 */

int tflm_titan_intent_init(void);
void tflm_titan_intent_deinit(void);
int tflm_titan_intent_invoke(bool wake, const int8_t *features, size_t bytes,
                            struct titan_intent_score_s *score,
                            uint64_t *cycles, uint32_t *elapsed_us);
size_t tflm_titan_intent_arena(bool wake);
const char *tflm_titan_intent_version(void);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_INCLUDE_AUDIOUTILS_TITAN_INTENT_H */
