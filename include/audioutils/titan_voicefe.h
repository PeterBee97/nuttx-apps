/****************************************************************************
 * apps/include/audioutils/titan_voicefe.h
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

#ifndef __APPS_INCLUDE_AUDIOUTILS_TITAN_VOICEFE_H
#define __APPS_INCLUDE_AUDIOUTILS_TITAN_VOICEFE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdint.h>
#include <stddef.h>
#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VOICEFE_FRAMES 160
#define VOICEFE_DELAY  8
#define VOICEFE_LEFT   0
#define VOICEFE_RIGHT  1
#define VOICEFE_MEAN   2
#define VOICEFE_BEAM   3

#ifdef CONFIG_AUDIOUTILS_TITAN_VOICEFE_FEATURES
#define VOICEFE_MELS        40
#define VOICEFE_FEATURE_WIN 400
#define VOICEFE_FEATURE_HOP 320
#define VOICEFE_KWS_ROWS    99
#define VOICEFE_COMMAND_ROWS 199
#define VOICEFE_FEATURE_SILENCE (-128)
#endif

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct voicefe_dsp_s
{
  int32_t previous[2];
  int32_t dc[2];
  int32_t gain[2];
  int16_t history[2][VOICEFE_DELAY * 2 + 1];
  int16_t filtered[VOICEFE_FRAMES][2];
  uint32_t clipped[2];
  uint32_t energy;
  uint32_t noise;
  uint32_t blocks;
  int lag;
  unsigned int cursor;
  int mode;
};

#ifdef CONFIG_AUDIOUTILS_TITAN_VOICEFE_NS
struct voicefe_ns_s
{
  int32_t real[512];
  int32_t imag[512];
  uint32_t power[257];
  uint32_t noise[257];
  uint32_t minimum[257];
  uint16_t gain[257];
  int16_t previous[VOICEFE_FRAMES];
  int32_t overlap[VOICEFE_FRAMES];
  uint32_t blocks;
  uint32_t clipped;
  uint32_t gain_q15;
  uint16_t floor;
};
#endif

#ifdef CONFIG_AUDIOUTILS_TITAN_VOICEFE_FEATURES
struct voicefe_features_s
{
  int32_t real[512];
  int32_t imag[512];
  uint32_t power[257];
  int16_t pcm[VOICEFE_FEATURE_WIN + VOICEFE_FRAMES];
  unsigned int count;
  uint32_t frames;
};
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

void voicefe_dsp_init(struct voicefe_dsp_s *state, int mode,
                     int32_t left_gain, int32_t right_gain);
int voicefe_delay(const int16_t *stereo, size_t frames);
void voicefe_process(struct voicefe_dsp_s *state, const int16_t *stereo,
                     int16_t *mono);
int voicefe_speech(struct voicefe_dsp_s *state, const int16_t *mono);
uint32_t voicefe_isqrt(uint64_t value);

#ifdef CONFIG_AUDIOUTILS_TITAN_VOICEFE_NS
void voicefe_ns_init(struct voicefe_ns_s *state, uint16_t floor);
void voicefe_ns_process(struct voicefe_ns_s *state, int16_t *mono);
#endif

#ifdef CONFIG_AUDIOUTILS_TITAN_VOICEFE_FEATURES
/* Features are log2(power + 1) * 8 - 128, saturated to INT8. Model input
 * quantization is applied when copying this representation to tensors.
 * push returns one when a row is ready and zero while filling the window.
 */

void voicefe_features_init(struct voicefe_features_s *state);
int voicefe_features_push(struct voicefe_features_s *state,
                         const int16_t *mono, int8_t *row);
size_t voicefe_features_size(void);
#endif

#endif /* __APPS_INCLUDE_AUDIOUTILS_TITAN_VOICEFE_H */
