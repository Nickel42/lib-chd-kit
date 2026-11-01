# Rebuilds libchdkit.a on Apple platforms as a single prelinked object per
# architecture in which every symbol except the chdman_* C API is private.
# This stops the bundled zlib/zstd/FLAC/LZMA/MAME symbols from clashing with
# copies an application links itself.
#
#   cmake -DFILELIST=<objects.txt> -DARCHIVE=<libchdkit.a> -DARCHS="arm64;x86_64" -DMINOS=11.0 -DWORK=<dir> -P prelink_static.cmake

# stamp the platform so linkers don't warn about a missing LC_BUILD_VERSION
set(platform_args)
if(MINOS)
  execute_process(COMMAND xcrun --show-sdk-version OUTPUT_VARIABLE sdk OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0 OR NOT sdk)
    set(sdk ${MINOS})
  endif()
  set(platform_args -platform_version macos ${MINOS} ${sdk})
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(thin)
foreach(arch IN LISTS ARCHS)
  execute_process(
    COMMAND ld -r -arch ${arch} ${platform_args} -exported_symbol _chdman_* -filelist "${FILELIST}" -o "${WORK}/chdman_${arch}.o"
    RESULT_VARIABLE rc ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "prelink (${arch}) failed: ${err}")
  endif()
  list(APPEND thin "${WORK}/chdman_${arch}.o")
endforeach()
list(LENGTH thin count)
if(count GREATER 1)
  execute_process(COMMAND lipo -create ${thin} -output "${WORK}/chdman.o" RESULT_VARIABLE rc)
else()
  execute_process(COMMAND ${CMAKE_COMMAND} -E copy ${thin} "${WORK}/chdman.o" RESULT_VARIABLE rc)
endif()
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "lipo failed")
endif()
file(REMOVE "${ARCHIVE}")
execute_process(COMMAND libtool -static -no_warning_for_no_symbols -o "${ARCHIVE}" "${WORK}/chdman.o" RESULT_VARIABLE rc ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "libtool failed: ${err}")
endif()
