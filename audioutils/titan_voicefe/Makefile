############################################################################
# apps/audioutils/titan_voicefe/Makefile
#
# SPDX-License-Identifier: Apache-2.0
############################################################################

include $(APPDIR)/Make.defs

CSRCS = voicefe_dsp.c

ifeq ($(CONFIG_AUDIOUTILS_TITAN_VOICEFE_NS),y)
CSRCS += voicefe_ns.c
endif

ifneq ($(filter y,$(CONFIG_AUDIOUTILS_TITAN_VOICEFE_NS) $(CONFIG_AUDIOUTILS_TITAN_VOICEFE_FEATURES)),)
CSRCS += voicefe_fft.c
endif

ifeq ($(CONFIG_AUDIOUTILS_TITAN_VOICEFE_FEATURES),y)
CSRCS += voicefe_features.c
endif

include $(APPDIR)/Application.mk
