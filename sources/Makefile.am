
include $(top_srcdir)/build/modmake.rulesam

LIBWHISPER_INC=-I/opt/whisper_cpp/include
LIBWHISPER_LIB=-L/opt/whisper_cpp/lib -lwhisper

MODNAME = mod_whisper_asr
mod_LTLIBRARIES = mod_whisper_asr.la
mod_whisper_asr_la_SOURCES  = mod_whisper_asr.c utils.c
mod_whisper_asr_la_CFLAGS   = $(AM_CFLAGS) $(OFLAGS) -I. $(LIBWHISPER_INC) -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable -Wno-unused-label -Wno-declaration-after-statement -Wno-pointer-arith
mod_whisper_asr_la_LIBADD   = $(switch_builddir)/libfreeswitch.la $(LIBWHISPER_LIB)
mod_whisper_asr_la_LDFLAGS  = -avoid-version -module -no-undefined -shared

$(am_mod_whisper_asr_la_OBJECTS): mod_whisper_asr.h

