/****************************************************************************
 * apps/testing/titan_asr/asr_frontend.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef ASR_FRONTEND_H
#define ASR_FRONTEND_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdint.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Diagnostic singleton. mode: 0 raw mean, 1 AEC mean, 2 AEC beam,
 * 3 AEC beam and NS, 4 AEC beam and residual spectral suppression.
 * Dump records contain six signed 16-bit channels:
 * resampled L, R, reference, cancelled L, R, final mono.
 */

int asr_frontend_open(unsigned int mode, int dump);
int asr_frontend_push(const int16_t *input, int16_t *mono);
int asr_frontend_replay(const int16_t *input, int16_t *mono);
void asr_frontend_close(void);

#ifdef CONFIG_TESTING_TITAN_ASR_SNAPSHOT_TRACE
int asr_frontend_diagnostic(int suppression, int noise);
uint64_t asr_frontend_ticks(void);
#endif

#endif
