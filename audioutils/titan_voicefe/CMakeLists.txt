# ##############################################################################
# apps/audioutils/titan_voicefe/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

if(CONFIG_AUDIOUTILS_TITAN_VOICEFE)
  target_sources(apps PRIVATE voicefe_dsp.c)
  if(CONFIG_AUDIOUTILS_TITAN_VOICEFE_NS)
    target_sources(apps PRIVATE voicefe_ns.c)
  endif()
  if(CONFIG_AUDIOUTILS_TITAN_VOICEFE_NS
     OR CONFIG_AUDIOUTILS_TITAN_VOICEFE_FEATURES)
    target_sources(apps PRIVATE voicefe_fft.c)
  endif()
  if(CONFIG_AUDIOUTILS_TITAN_VOICEFE_FEATURES)
    target_sources(apps PRIVATE voicefe_features.c)
  endif()
endif()
