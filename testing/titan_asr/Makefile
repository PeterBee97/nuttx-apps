include $(APPDIR)/Make.defs

PROGNAME = $(CONFIG_TESTING_TITAN_ASR_PROGNAME)
PRIORITY = $(CONFIG_TESTING_TITAN_ASR_PRIORITY)
STACKSIZE = $(CONFIG_TESTING_TITAN_ASR_STACKSIZE)
MODULE = $(CONFIG_TESTING_TITAN_ASR)
MAINSRC = titan_asr_main.c
CSRCS = asr_ctc.c asr_runtime.c asr_stream.c board_math.c
ifeq ($(CONFIG_TESTING_TITAN_ASR_MVE),y)
CSRCS += asr_mve.c
asr_mve.c_CFLAGS += -march=armv8.1-m.main+mve.fp -mfpu=auto
endif
ifeq ($(CONFIG_TESTING_TITAN_ASR_OPT),y)
CSRCS += asr_bench.c
endif
CXXEXT = .cc
CXXSRCS = board_model.cc
ASRCS = board_data.S
AFLAGS += -DASR_MODEL0=\"$(CURDIR)/model0.tflite\"
AFLAGS += -DASR_MODEL1=\"$(CURDIR)/model1.tflite\"
AFLAGS += -DASR_LM=\"$(CURDIR)/lm.bin\"
AFLAGS += -DASR_FIXTURES=\"$(CURDIR)/fixtures.bin\"
CFLAGS += -I$(CURDIR)
CXXFLAGS += -I$(CURDIR)
CXXFLAGS += -I$(APPDIR)/mlearning/tflite-micro/tflite-micro
CXXFLAGS += -I$(APPDIR)/math/gemmlowp/gemmlowp
CXXFLAGS += -DTF_LITE_STATIC_MEMORY -DTFLITE_EMULATE_FLOAT
CXXFLAGS += -DTF_LITE_STRIP_ERROR_STRINGS -Wno-shadow -Wno-undef

include $(APPDIR)/Application.mk

$(CAOBJS): $(CURDIR)/model0.tflite $(CURDIR)/model1.tflite \
           $(CURDIR)/lm.bin $(CURDIR)/fixtures.bin
