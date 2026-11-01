# Vendored third-party and MAME utility code for lib-chd-kit.
# Everything here is built as position-independent object libraries with
# hidden visibility, then folded into lib-chd-kit (static and shared).
#
# On MSVC the DLL and the static library can use different C runtimes
# (CHDMAN_SHARED_MSVC_RUNTIME / CHDMAN_STATIC_MSVC_RUNTIME). Objects built for
# one runtime can't be linked with the other, so in that case the vendored
# code is compiled twice: once as the primary set (target names without a
# suffix) and once more with the suffix in CHDMAN_STATIC_VENDOR_SUFFIX.

# MSVC switches MAME's own build uses (scripts/genie.lua); MAME 0.289 needs
# Visual Studio 2022 17.5 or newer for these and for C++20.
function(chdman_msvc_options tgt)
  if(MSVC)
    target_compile_options(${tgt} PRIVATE
      /bigobj /utf-8 /Zc:preprocessor
      "$<$<COMPILE_LANGUAGE:CXX>:/EHsc;/Zc:__cplusplus;/Zc:enumTypes;/Zc:templateScope;/Zc:u8EscapeEncoding>")
  endif()
endfunction()

function(chdman_vendor_target tgt runtime)
  set_target_properties(${tgt} PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    FOLDER vendor)
  if(runtime)
    set_target_properties(${tgt} PROPERTIES MSVC_RUNTIME_LIBRARY "${runtime}")
  endif()
  chdman_msvc_options(${tgt})
  if(MSVC)
    target_compile_options(${tgt} PRIVATE /W0)
  else()
    target_compile_options(${tgt} PRIVATE -w)
  endif()
endfunction()

# Defines chdman_{zlib,lzma,zstd,flac,utf8proc,mame}<sfx> built for <runtime>.
function(chdman_add_vendor_set sfx runtime)
  set(MAME ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/mame)

  # -------------------------------------------------------------- zlib (Zlib)
  add_library(chdman_zlib${sfx} OBJECT
    ${MAME}/3rdparty/zlib/adler32.c
    ${MAME}/3rdparty/zlib/compress.c
    ${MAME}/3rdparty/zlib/crc32.c
    ${MAME}/3rdparty/zlib/deflate.c
    ${MAME}/3rdparty/zlib/inffast.c
    ${MAME}/3rdparty/zlib/inflate.c
    ${MAME}/3rdparty/zlib/inftrees.c
    ${MAME}/3rdparty/zlib/trees.c
    ${MAME}/3rdparty/zlib/uncompr.c
    ${MAME}/3rdparty/zlib/zutil.c)
  target_include_directories(chdman_zlib${sfx} PUBLIC ${MAME}/3rdparty/zlib)
  target_compile_definitions(chdman_zlib${sfx} PUBLIC ZLIB_CONST PRIVATE $<$<NOT:$<BOOL:${WIN32}>>:HAVE_UNISTD_H>)
  chdman_vendor_target(chdman_zlib${sfx} "${runtime}")

  # ----------------------------------------------- LZMA SDK (public domain)
  add_library(chdman_lzma${sfx} OBJECT
    ${MAME}/3rdparty/lzma/C/LzmaDec.c
    ${MAME}/3rdparty/lzma/C/LzmaEnc.c
    ${MAME}/3rdparty/lzma/C/LzFind.c
    ${MAME}/3rdparty/lzma/C/LzFindOpt.c
    ${MAME}/3rdparty/lzma/C/CpuArch.c)
  target_include_directories(chdman_lzma${sfx} PUBLIC ${MAME}/3rdparty)
  target_compile_definitions(chdman_lzma${sfx} PUBLIC Z7_ST Z7_PPMD_SUPPORT)
  chdman_vendor_target(chdman_lzma${sfx} "${runtime}")

  # ---------------------------------------------- zstd (BSD-3-Clause option)
  file(GLOB zstd_sources
    ${MAME}/3rdparty/zstd/lib/common/*.c
    ${MAME}/3rdparty/zstd/lib/compress/*.c
    ${MAME}/3rdparty/zstd/lib/decompress/*.c)
  add_library(chdman_zstd${sfx} OBJECT ${zstd_sources})
  target_include_directories(chdman_zstd${sfx} PUBLIC ${MAME}/3rdparty/zstd/lib)
  # zstd forces default visibility on its API; keep it internal to lib-chd-kit
  target_compile_definitions(chdman_zstd${sfx} PUBLIC ZSTD_DISABLE_ASM XXH_NAMESPACE=ZSTD_ ZSTDLIB_VISIBLE= ZSTDERRORLIB_VISIBLE= ZDICTLIB_VISIBLE= PRIVATE ZSTD_MULTITHREAD=0)
  chdman_vendor_target(chdman_zstd${sfx} "${runtime}")

  # ------------------------------------------- libFLAC (BSD-3-Clause, Xiph)
  set(FLAC_SRC ${MAME}/3rdparty/flac/src/libFLAC)
  add_library(chdman_flac${sfx} OBJECT
    ${FLAC_SRC}/bitmath.c ${FLAC_SRC}/bitreader.c ${FLAC_SRC}/bitwriter.c
    ${FLAC_SRC}/cpu.c ${FLAC_SRC}/crc.c ${FLAC_SRC}/fixed.c
    ${FLAC_SRC}/fixed_intrin_avx2.c ${FLAC_SRC}/fixed_intrin_sse2.c
    ${FLAC_SRC}/fixed_intrin_sse42.c ${FLAC_SRC}/fixed_intrin_ssse3.c
    ${FLAC_SRC}/float.c ${FLAC_SRC}/format.c ${FLAC_SRC}/lpc.c
    ${FLAC_SRC}/lpc_intrin_avx2.c ${FLAC_SRC}/lpc_intrin_fma.c
    ${FLAC_SRC}/lpc_intrin_neon.c ${FLAC_SRC}/lpc_intrin_sse2.c
    ${FLAC_SRC}/lpc_intrin_sse41.c ${FLAC_SRC}/md5.c ${FLAC_SRC}/memory.c
    ${FLAC_SRC}/stream_decoder.c ${FLAC_SRC}/stream_encoder.c
    ${FLAC_SRC}/stream_encoder_framing.c
    ${FLAC_SRC}/stream_encoder_intrin_avx2.c
    ${FLAC_SRC}/stream_encoder_intrin_sse2.c
    ${FLAC_SRC}/stream_encoder_intrin_ssse3.c
    ${FLAC_SRC}/window.c)
  if(WIN32)
    # UTF-8 fopen() wrapper libFLAC uses on Windows (as in MAME's build)
    target_sources(chdman_flac${sfx} PRIVATE ${MAME}/3rdparty/flac/src/share/win_utf8_io/win_utf8_io.c)
  endif()
  target_include_directories(chdman_flac${sfx}
    PUBLIC ${MAME}/3rdparty/flac/include
    PRIVATE ${FLAC_SRC}/include)
  target_compile_definitions(chdman_flac${sfx}
    PUBLIC FLAC__NO_DLL
    PRIVATE
      NDEBUG HAVE_CONFIG_H ENABLE_64_BIT_WORDS=1 OGG_FOUND=0 FLAC__HAS_OGG=0
      HAVE_LROUND=1 HAVE_INTTYPES_H HAVE_STDBOOL_H HAVE_STDINT_H HAVE_STDIO_H
      HAVE_STDLIB_H HAVE_STRING_H _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE
      CPU_IS_BIG_ENDIAN=0 CPU_IS_LITTLE_ENDIAN=1 WORDS_BIGENDIAN=0
      $<$<BOOL:${APPLE}>:FLAC__SYS_DARWIN>)
  chdman_vendor_target(chdman_flac${sfx} "${runtime}")

  # --------------------------------------------------------- utf8proc (MIT)
  add_library(chdman_utf8proc${sfx} OBJECT ${MAME}/3rdparty/utf8proc/utf8proc.c)
  target_include_directories(chdman_utf8proc${sfx} PUBLIC ${MAME}/3rdparty/utf8proc)
  target_compile_definitions(chdman_utf8proc${sfx} PUBLIC UTF8PROC_STATIC)
  chdman_vendor_target(chdman_utf8proc${sfx} "${runtime}")

  # ----------------------------------- MAME util + osd subset (BSD-3-Clause)
  set(UTIL ${MAME}/src/lib/util)
  set(OSD ${MAME}/src/osd)
  set(mame_sources
    ${UTIL}/avhuff.cpp ${UTIL}/aviio.cpp ${UTIL}/bitmap.cpp ${UTIL}/cdrom.cpp
    ${UTIL}/chd.cpp ${UTIL}/chdcodec.cpp ${UTIL}/corealloc.cpp
    ${UTIL}/corefile.cpp ${UTIL}/corestr.cpp ${UTIL}/coreutil.cpp
    ${UTIL}/dvdrom.cpp ${UTIL}/flac.cpp ${UTIL}/hashing.cpp
    ${UTIL}/huffman.cpp ${UTIL}/ioprocs.cpp ${UTIL}/md5.cpp ${UTIL}/palette.cpp
    ${UTIL}/path.cpp ${UTIL}/strformat.cpp ${UTIL}/timeconv.cpp
    ${UTIL}/unicode.cpp ${UTIL}/vbiparse.cpp ${UTIL}/vecstream.cpp
    ${OSD}/osdcore.cpp ${OSD}/osdsync.cpp ${OSD}/strconv.cpp
    ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/osd_shim.cpp)
  if(WIN32)
    list(APPEND mame_sources
      ${OSD}/modules/file/winfile.cpp ${OSD}/modules/file/windir.cpp
      ${OSD}/windows/winutil.cpp ${OSD}/windows/winutf8.cpp)
  else()
    list(APPEND mame_sources
      ${OSD}/modules/file/posixfile.cpp ${OSD}/modules/file/posixdir.cpp)
  endif()

  add_library(chdman_mame${sfx} OBJECT ${mame_sources})
  target_include_directories(chdman_mame${sfx} PUBLIC ${UTIL} ${MAME}/src/lib ${OSD} ${OSD}/modules/file ${OSD}/modules/lib ${MAME}/3rdparty
    PRIVATE ${OSD}/windows)
  target_compile_definitions(chdman_mame${sfx} PUBLIC LSB_FIRST NDEBUG $<IF:$<BOOL:${WIN32}>,CRLF=3,CRLF=2>
    $<$<BOOL:${WIN32}>:OSD_WINDOWS WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00 _CRT_SECURE_NO_WARNINGS>
    $<$<NOT:$<BOOL:${WIN32}>>:SDLMAME_UNIX>
    $<$<BOOL:${APPLE}>:SDLMAME_MACOSX SDLMAME_DARWIN>)
  target_link_libraries(chdman_mame${sfx} PUBLIC chdman_zlib${sfx} chdman_lzma${sfx} chdman_zstd${sfx} chdman_flac${sfx} chdman_utf8proc${sfx})
  target_compile_features(chdman_mame${sfx} PUBLIC cxx_std_20)
  chdman_vendor_target(chdman_mame${sfx} "${runtime}")
endfunction()

# Sets <out> to the object files of the vendor set with suffix <sfx>.
function(chdman_vendor_objects out sfx)
  set(${out}
    $<TARGET_OBJECTS:chdman_mame${sfx}>
    $<TARGET_OBJECTS:chdman_zlib${sfx}>
    $<TARGET_OBJECTS:chdman_lzma${sfx}>
    $<TARGET_OBJECTS:chdman_zstd${sfx}>
    $<TARGET_OBJECTS:chdman_flac${sfx}>
    $<TARGET_OBJECTS:chdman_utf8proc${sfx}>
    PARENT_SCOPE)
endfunction()

# Primary set: the DLL/dylib, CLI and upstream reference link against it
# (or the static library when no shared library is built).
chdman_add_vendor_set("" "${CHDMAN_PRIMARY_MSVC_RUNTIME}")

set(CHDMAN_STATIC_VENDOR_SUFFIX "" PARENT_SCOPE)
if(MSVC AND CHDMAN_BUILD_SHARED AND CHDMAN_BUILD_STATIC
   AND NOT CHDMAN_STATIC_MSVC_RUNTIME STREQUAL CHDMAN_SHARED_MSVC_RUNTIME)
  chdman_add_vendor_set("_static" "${CHDMAN_STATIC_MSVC_RUNTIME}")
  set(CHDMAN_STATIC_VENDOR_SUFFIX "_static" PARENT_SCOPE)
endif()
