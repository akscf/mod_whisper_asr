
include $(top_srcdir)/build/modmake.rulesam

OFLAGS=-march=westmere -msse3
MODNAME = mod_whisper_asr
WHISPER_SRC = ./whisper/ggml.c ./whisper/common-ggml.cpp ./whisper/common.cpp ./whisper/whisper.cpp
mod_LTLIBRARIES = mod_whisper_asr.la
mod_whisper_asr_la_SOURCES  = mod_whisper_asr.c utils.c whisper_api.cpp $(WHISPER_SRC)
mod_whisper_asr_la_CFLAGS   = $(AM_CFLAGS) $(OFLAGS) -I. -I./whisper -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable -Wno-unused-label -Wno-declaration-after-statement
mod_whisper_asr_la_CXXFLAGS = $(AM_CXXFLAGS) $(OFLAGS) -I. -I./whisper
mod_whisper_asr_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_whisper_asr_la_LDFLAGS  = -avoid-version -module -no-undefined -shared

$(am_mod_whisper_asr_la_OBJECTS): mod_whisper_asr.h

