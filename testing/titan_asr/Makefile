include $(APPDIR)/Make.defs

PROGNAME = $(CONFIG_TESTING_TITAN_ASR_PROGNAME)
PRIORITY = $(CONFIG_TESTING_TITAN_ASR_PRIORITY)
STACKSIZE = $(CONFIG_TESTING_TITAN_ASR_STACKSIZE)
MODULE = $(CONFIG_TESTING_TITAN_ASR)
MAINSRC = titan_asr_main.c
CSRCS = asr_ctc.c asr_runtime.c asr_stream.c board_math.c
ifeq ($(CONFIG_TESTING_TITAN_DEMO),y)
CSRCS += titan_demo.c titan_media.c
endif
ifeq ($(CONFIG_TESTING_TITAN_DEMO_STORAGE),y)
CSRCS += titan_storage.c
endif
ifeq ($(CONFIG_TESTING_TITAN_ASR_DUPLEX),y)
CSRCS += asr_frontend.c speex/mdf.c speex/fftwrap.c
CSRCS += speex/kiss_fft.c speex/kiss_fftr.c
CFLAGS += -I$(CURDIR)/speex -DHAVE_CONFIG_H
asr_frontend.c_CFLAGS += -O2
speex/mdf.c_CFLAGS += -O2
speex/fftwrap.c_CFLAGS += -O2
speex/kiss_fft.c_CFLAGS += -O2
speex/kiss_fftr.c_CFLAGS += -O2
ifeq ($(CONFIG_TESTING_TITAN_ASR_AEC_O3),y)
speex/mdf.c_CFLAGS += -O3
speex/fftwrap.c_CFLAGS += -O3
speex/kiss_fft.c_CFLAGS += -O3
speex/kiss_fftr.c_CFLAGS += -O3
endif
ifeq ($(CONFIG_TESTING_TITAN_ASR_RESIDUAL),y)
CSRCS += speex/preprocess.c speex/filterbank.c
speex/preprocess.c_CFLAGS += -O2
speex/filterbank.c_CFLAGS += -O2
endif
endif
ifeq ($(CONFIG_TESTING_TITAN_ASR_MVE),y)
CSRCS += asr_mve.c
asr_mve.c_CFLAGS += -march=armv8.1-m.main+mve.fp -mfpu=auto
# Application.mk batches dependency scans without per-source flags.
$(PREFIX).depend: CFLAGS += -march=armv8.1-m.main+mve.fp -mfpu=auto
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
