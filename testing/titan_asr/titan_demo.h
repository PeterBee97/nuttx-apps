/****************************************************************************
 * apps/testing/titan_asr/titan_demo.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef TITAN_DEMO_H
#define TITAN_DEMO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <audioutils/titan_intent.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int titan_demo_open(const char *directory);
void titan_demo_close(void);
bool titan_demo_stopping(void);
void titan_demo_stop(void);
void titan_demo_status(void);
void titan_demo_event(const struct titan_intent_event_s *event);
void titan_demo_vad(bool speech, bool authorized);
void titan_demo_pcm(const int16_t *pcm);
void titan_demo_sync(struct titan_intent_state_s *state);

#ifdef CONFIG_TESTING_TITAN_DEMO_STORAGE
int titan_storage_start(void);
#endif

#endif /* TITAN_DEMO_H */
