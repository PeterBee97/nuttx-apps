/****************************************************************************
 * apps/system/titan_voice/voicefe_intent.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APPS_SYSTEM_TITAN_VOICE_VOICEFE_INTENT_H
#define __APPS_SYSTEM_TITAN_VOICE_VOICEFE_INTENT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <audioutils/titan_intent.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct voicefe_intent_stats_s
{
  struct titan_intent_state_s state;
  struct titan_intent_score_s last_command;
  uint64_t wake_cycles;
  uint64_t command_cycles;
  uint32_t wake_us;
  uint32_t command_us;
  uint32_t wake_invokes;
  uint32_t command_invokes;
  uint32_t failures;
  uint32_t overruns;
  uint32_t highwater;
  uint32_t missed_wake;
  uint32_t snapshot_busy;
  uint32_t event_drop;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int voicefe_intent_start(void);
void voicefe_intent_stop(void);
void voicefe_intent_reset(void);
bool voicefe_intent_upload(void);
uint32_t voicefe_intent_generation(void);
void voicefe_intent_put(const int16_t *pcm, uint32_t sequence,
                       uint64_t utterance, uint64_t start_sample,
                       bool speech, bool marker_end);
void voicefe_intent_stats(struct voicefe_intent_stats_s *stats);
bool voicefe_intent_poll(struct titan_intent_event_s *event);
void voicefe_intent_print(bool events);

#endif /* __APPS_SYSTEM_TITAN_VOICE_VOICEFE_INTENT_H */
