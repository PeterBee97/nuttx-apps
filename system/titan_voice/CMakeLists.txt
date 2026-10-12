# ##############################################################################
# apps/system/titan_voice/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

if(CONFIG_SYSTEM_TITAN_VOICE)
  set(VOICEFE_SRCS voicefe.c)
  if(CONFIG_SYSTEM_TITAN_VOICE_INTENT)
    list(APPEND VOICEFE_SRCS voicefe_intent.c)
  endif()
  if(CONFIG_SYSTEM_TITAN_VOICE_INTENT_TELEMETRY)
    list(APPEND VOICEFE_SRCS voicefe_telemetry.c)
  endif()
  nuttx_add_application(
    NAME
    voicefe
    SRCS
    ${VOICEFE_SRCS}
    STACKSIZE
    4096
    PRIORITY
    ${CONFIG_SYSTEM_TITAN_VOICE_NET_PRIORITY})
endif()
