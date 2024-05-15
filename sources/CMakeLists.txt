cmake_minimum_required(VERSION 3.18)
project(mod_whisper_asr)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_SHARED_LIBRARY_PREFIX "")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g")

#set(ENV{PKG_CONFIG_PATH} "/usr/local/freeswitch/lib/pkgconfig:/usr/local/ssl/lib/pkgconfig/:$ENV{PKG_CONFIG_PATH}")

find_package(PkgConfig REQUIRED)
pkg_check_modules(FreeSWITCH REQUIRED IMPORTED_TARGET freeswitch)
pkg_get_variable(FS_MOD_DIR freeswitch modulesdir)
message("FreeSWITCH modules dir: ${FS_MOD_DIR}")

if(CMAKE_BUILD_TYPE MATCHES "Release")
    # Add linker flags to strip symbols and reduce size of static library
    set_target_properties(PROPERTIES LINK_FLAGS_RELEASE "-s -w") #-static-libgcc -static-libstdc++
endif()

add_library(mod_whisper_asr SHARED mod_whisper_asr.c mod_whisper_asr.h whisper_api.h whisper_api.cpp utils.c whisper/ggml.h whisper/ggml.c whisper/whisper.h whisper/whisper.cpp)

set_property(TARGET mod_whisper_asr PROPERTY POSITION_INDEPENDENT_CODE ON)

target_include_directories(mod_whisper_asr PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/whisper)

target_link_libraries(mod_whisper_asr PRIVATE PkgConfig::FreeSWITCH pthread)

install(TARGETS mod_whisper_asr DESTINATION ${FS_MOD_DIR})