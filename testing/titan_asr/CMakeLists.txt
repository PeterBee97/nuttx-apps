# SPDX-License-Identifier: Apache-2.0

if(CONFIG_TESTING_TITAN_ASR)
  set_source_files_properties(
    board_data.S
    PROPERTIES
      OBJECT_DEPENDS
      "${CMAKE_CURRENT_LIST_DIR}/model0.tflite;${CMAKE_CURRENT_LIST_DIR}/model1.tflite;${CMAKE_CURRENT_LIST_DIR}/lm.bin;${CMAKE_CURRENT_LIST_DIR}/fixtures.bin"
      COMPILE_DEFINITIONS
      "ASR_MODEL0=\"${CMAKE_CURRENT_LIST_DIR}/model0.tflite\";ASR_MODEL1=\"${CMAKE_CURRENT_LIST_DIR}/model1.tflite\";ASR_LM=\"${CMAKE_CURRENT_LIST_DIR}/lm.bin\";ASR_FIXTURES=\"${CMAKE_CURRENT_LIST_DIR}/fixtures.bin\""
  )
  nuttx_add_application(
    NAME
    ${CONFIG_TESTING_TITAN_ASR_PROGNAME}
    STACKSIZE
    ${CONFIG_TESTING_TITAN_ASR_STACKSIZE}
    PRIORITY
    ${CONFIG_TESTING_TITAN_ASR_PRIORITY}
    MODULE
    ${CONFIG_TESTING_TITAN_ASR}
    SRCS
    titan_asr_main.c
    asr_ctc.c
    asr_runtime.c
    asr_stream.c
    board_math.c
    board_model.cc
    board_data.S
    INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_LIST_DIR}
    ${NUTTX_APPS_DIR}/mlearning/tflite-micro/tflite-micro
    ${NUTTX_APPS_DIR}/math/gemmlowp/gemmlowp
    ${NUTTX_APPS_DIR}/system/flatbuffers/flatbuffers/include
    COMPILE_FLAGS
    -DTF_LITE_STATIC_MEMORY
    -DTFLITE_EMULATE_FLOAT
    -DTF_LITE_STRIP_ERROR_STRINGS
    -Wno-shadow
    -Wno-undef)
  if(CONFIG_TESTING_TITAN_ASR_MVE)
    target_sources(apps_titan_asr PRIVATE asr_mve.c)
    set_source_files_properties(
      asr_mve.c PROPERTIES COMPILE_OPTIONS
      "-march=armv8.1-m.main+mve.fp;-mfpu=auto")
  endif()
  if(CONFIG_TESTING_TITAN_ASR_OPT)
    target_sources(apps_titan_asr PRIVATE asr_bench.c)
  endif()
endif()
