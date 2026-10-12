# ##############################################################################
# apps/audioutils/titan_intent/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

if(CONFIG_AUDIOUTILS_TITAN_INTENT)
  target_sources(apps PRIVATE intent_gate.c intent_state.c)
endif()
