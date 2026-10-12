/****************************************************************************
 * apps/system/titan_voice/voicefe_telemetry.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APPS_SYSTEM_TITAN_VOICE_VOICEFE_TELEMETRY_H
#define __APPS_SYSTEM_TITAN_VOICE_VOICEFE_TELEMETRY_H

void voicefe_telemetry_init(void);
void voicefe_telemetry_tick(const char *host, int port, const char *boot);

#endif /* __APPS_SYSTEM_TITAN_VOICE_VOICEFE_TELEMETRY_H */
