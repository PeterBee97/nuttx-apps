############################################################################
# apps/system/titan_voice/Makefile
#
# SPDX-License-Identifier: Apache-2.0
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = voicefe
PRIORITY  = $(CONFIG_SYSTEM_TITAN_VOICE_NET_PRIORITY)
STACKSIZE = 4096
MODULE    = $(CONFIG_SYSTEM_TITAN_VOICE)
MAINSRC   = voicefe.c

ifeq ($(CONFIG_SYSTEM_TITAN_VOICE_INTENT),y)
CSRCS += voicefe_intent.c
endif

ifeq ($(CONFIG_SYSTEM_TITAN_VOICE_INTENT_TELEMETRY),y)
CSRCS += voicefe_telemetry.c
endif

include $(APPDIR)/Application.mk
