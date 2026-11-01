cmake_minimum_required(VERSION 3.20)

# Default macOS deployment target / architectures must be set before project().
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

project(lib-chd-kit VERSION 1.0.0 DESCRIPTION "MAME 0.289 CHD tools as a library" LANGUAGES C CXX)
set(CHDMAN_MAME_VERSION "0.289")

option(CHDMAN_BUILD_SHARED "Build lib-chd-kit as a shared library (dylib/dll)" ON)
option(CHDMAN_BUILD_STATIC "Build lib-chd-kit as a static library" ON)
option(CHDMAN_BUILD_CLI "Build the chdman-cli command line tool" ON)
option(CHDMAN_BUILD_TESTS "Build the test suite" ON)
option(CHDMAN_STATIC_PRELINK "Apple: prelink the static library so only chdman_* symbols are global" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
# MSVC C runtime. The DLL defaults to the static CRT, so chdkit.dll needs no
# VC++ redistributable. The static library defaults to the DLL CRT (/MD),
# which is what Visual Studio projects use, so it links into a typical app
# without LNK2038 runtime mismatches. Passing CMAKE_MSVC_RUNTIME_LIBRARY sets
# the default for both.
if(DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
  set(_chdman_shared_rt "${CMAKE_MSVC_RUNTIME_LIBRARY}")
  set(_chdman_static_rt "${CMAKE_MSVC_RUNTIME_LIBRARY}")
else()
  set(_chdman_shared_rt "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set(_chdman_static_rt "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
set(CHDMAN_SHARED_MSVC_RUNTIME "${_chdman_shared_rt}" CACHE STRING "MSVC runtime for chdkit.dll and chdman-cli")
set(CHDMAN_STATIC_MSVC_RUNTIME "${_chdman_static_rt}" CACHE STRING "MSVC runtime for chdkit_static.lib")
if(CHDMAN_BUILD_SHARED)
  set(CHDMAN_PRIMARY_MSVC_RUNTIME "${CHDMAN_SHARED_MSVC_RUNTIME}")
else()
  set(CHDMAN_PRIMARY_MSVC_RUNTIME "${CHDMAN_STATIC_MSVC_RUNTIME}")
endif()
# targets that don't pick a runtime explicitly (CLI, tests, upstream reference)
set(CMAKE_MSVC_RUNTIME_LIBRARY "${CHDMAN_PRIMARY_MSVC_RUNTIME}")

add_subdirectory(vendor)

set(CHDMAN_SOURCES
  src/chdman_core.cpp
  src/chdman_capi.cpp)

chdman_vendor_objects(CHDMAN_VENDOR_OBJECTS "")
chdman_vendor_objects(CHDMAN_STATIC_VENDOR_OBJECTS "${CHDMAN_STATIC_VENDOR_SUFFIX}")

function(chdman_configure_library tgt)
  target_include_directories(${tgt}
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>
    PRIVATE src)
  # usage requirements of the vendored object libraries (includes/defines)
  target_include_directories(${tgt} PRIVATE
    $<TARGET_PROPERTY:chdman_mame,INTERFACE_INCLUDE_DIRECTORIES>)
  target_compile_definitions(${tgt} PRIVATE
    $<TARGET_PROPERTY:chdman_mame,INTERFACE_COMPILE_DEFINITIONS>
    $<TARGET_PROPERTY:chdman_zlib,INTERFACE_COMPILE_DEFINITIONS>
    $<TARGET_PROPERTY:chdman_flac,INTERFACE_COMPILE_DEFINITIONS>
    $<TARGET_PROPERTY:chdman_utf8proc,INTERFACE_COMPILE_DEFINITIONS>
    $<TARGET_PROPERTY:chdman_zstd,INTERFACE_COMPILE_DEFINITIONS>
    $<TARGET_PROPERTY:chdman_lzma,INTERFACE_COMPILE_DEFINITIONS>)
  set_target_properties(${tgt} PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    VERSION ${PROJECT_VERSION}
    SOVERSION 1)
  chdman_msvc_options(${tgt})
  if(MSVC)
    target_compile_options(${tgt} PRIVATE /W3)
  else()
    target_compile_options(${tgt} PRIVATE -Wall -Wno-sign-compare -Wno-unused-variable)
  endif()
  if(WIN32)
    target_link_libraries(${tgt} PRIVATE shlwapi)
  elseif(NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(${tgt} PRIVATE Threads::Threads)
  endif()
endfunction()

set(CHDMAN_INSTALL_TARGETS)

if(CHDMAN_BUILD_SHARED)
  add_library(chdman_shared SHARED ${CHDMAN_SOURCES} ${CHDMAN_VENDOR_OBJECTS})
  chdman_configure_library(chdman_shared)
  target_compile_definitions(chdman_shared PRIVATE CHDMAN_BUILDING_SHARED)
  set_target_properties(chdman_shared PROPERTIES OUTPUT_NAME chdkit)
  if(APPLE)
    set_target_properties(chdman_shared PROPERTIES
      INSTALL_NAME_DIR "@rpath"
      BUILD_WITH_INSTALL_NAME_DIR ON)
    # export only the C API, whatever the vendored headers ask for
    target_link_options(chdman_shared PRIVATE "LINKER:-exported_symbol,_chdman_*")
  elseif(UNIX)
    # same on ELF platforms: hidden visibility still leaves libstdc++'s
    # template instantiations exported, so a version script keeps only chdman_*
    target_link_options(chdman_shared PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/chdman.map")
    set_target_properties(chdman_shared PROPERTIES LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/chdman.map")
  endif()
  # same names as the installed package (find_package(chdman))
  add_library(chdman::chdman_shared ALIAS chdman_shared)
  add_library(chdman::shared ALIAS chdman_shared)
  list(APPEND CHDMAN_INSTALL_TARGETS chdman_shared)
endif()

if(CHDMAN_BUILD_STATIC)
  add_library(chdman_static STATIC ${CHDMAN_SOURCES} ${CHDMAN_STATIC_VENDOR_OBJECTS})
  chdman_configure_library(chdman_static)
  set_target_properties(chdman_static PROPERTIES MSVC_RUNTIME_LIBRARY "${CHDMAN_STATIC_MSVC_RUNTIME}")
  target_compile_definitions(chdman_static PRIVATE CHDMAN_BUILDING_STATIC PUBLIC CHDMAN_STATIC)
  # avoid chdkit.lib clashing with the DLL import library on Windows
  set_target_properties(chdman_static PROPERTIES OUTPUT_NAME $<IF:$<BOOL:${WIN32}>,chdkit_static,chdkit>)
  if(APPLE)
    # the static archive references libc++
    target_link_libraries(chdman_static INTERFACE c++)
  elseif(NOT WIN32)
    target_link_libraries(chdman_static INTERFACE stdc++ Threads::Threads)
  endif()
  if(APPLE AND CHDMAN_STATIC_PRELINK)
    set(_archs ${CMAKE_OSX_ARCHITECTURES})
    if(NOT _archs)
      set(_archs ${CMAKE_HOST_SYSTEM_PROCESSOR})
    endif()
    set(_objlist "${CMAKE_CURRENT_BINARY_DIR}/chdman_static_objects_$<CONFIG>.txt")
    file(GENERATE OUTPUT "${_objlist}" CONTENT
"$<JOIN:$<TARGET_OBJECTS:chdman_static>,\n>
$<JOIN:$<TARGET_OBJECTS:chdman_mame>,\n>
$<JOIN:$<TARGET_OBJECTS:chdman_zlib>,\n>
$<JOIN:$<TARGET_OBJECTS:chdman_lzma>,\n>
$<JOIN:$<TARGET_OBJECTS:chdman_zstd>,\n>
$<JOIN:$<TARGET_OBJECTS:chdman_flac>,\n>
$<JOIN:$<TARGET_OBJECTS:chdman_utf8proc>,\n>
")
    add_custom_command(TARGET chdman_static POST_BUILD
      COMMAND ${CMAKE_COMMAND} "-DFILELIST=${_objlist}" "-DARCHIVE=$<TARGET_FILE:chdman_static>"
        "-DARCHS=${_archs}" "-DMINOS=${CMAKE_OSX_DEPLOYMENT_TARGET}" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/prelink_$<CONFIG>"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/prelink_static.cmake
      COMMENT "Prelinking libchdkit.a (hiding non-API symbols)"
      VERBATIM)
  endif()
  add_library(chdman::chdman_static ALIAS chdman_static)
  add_library(chdman::static ALIAS chdman_static)
  list(APPEND CHDMAN_INSTALL_TARGETS chdman_static)
endif()

if(TARGET chdman_shared)
  set(CHDMAN_LINK_TARGET chdman_shared)
else()
  set(CHDMAN_LINK_TARGET chdman_static)
endif()

if(CHDMAN_BUILD_CLI)
  add_executable(chdman-cli cli/chdman_cli.c)
  target_link_libraries(chdman-cli PRIVATE ${CHDMAN_LINK_TARGET})
  chdman_msvc_options(chdman-cli)
  if(APPLE)
    set_target_properties(chdman-cli PROPERTIES BUILD_RPATH "@executable_path" INSTALL_RPATH "@executable_path/../lib")
  elseif(UNIX)
    set_target_properties(chdman-cli PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN/../lib")
  endif()
  list(APPEND CHDMAN_INSTALL_TARGETS chdman-cli)
endif()

if(CHDMAN_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

include(GNUInstallDirs)
install(TARGETS ${CHDMAN_INSTALL_TARGETS}
  EXPORT chdmanTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/chdman.h include/module.modulemap DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES THIRD_PARTY_NOTICES.md LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/lib-chd-kit)
install(EXPORT chdmanTargets NAMESPACE chdman:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/chdman FILE chdmanConfig.cmake)

# Optional: build upstream MAME src/tools/chdman.cpp unchanged from the
# vendored sources. Used only to check that lib-chd-kit produces byte-identical
# CHDs to the reference tool (see tests/parity.cmake).
option(CHDMAN_BUILD_UPSTREAM_REFERENCE "Build unmodified upstream chdman for parity tests" OFF)
if(CHDMAN_BUILD_UPSTREAM_REFERENCE)
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/upstream_version.cpp "extern const char build_version[];\nconst char build_version[] = \"0.289 (mame0289)\";\n")
  add_executable(chdman-upstream vendor/mame/src/tools/chdman.cpp ${CMAKE_CURRENT_BINARY_DIR}/upstream_version.cpp ${CHDMAN_VENDOR_OBJECTS})
  target_include_directories(chdman-upstream PRIVATE $<TARGET_PROPERTY:chdman_mame,INTERFACE_INCLUDE_DIRECTORIES>)
  target_compile_definitions(chdman-upstream PRIVATE $<TARGET_PROPERTY:chdman_mame,INTERFACE_COMPILE_DEFINITIONS>)
  chdman_msvc_options(chdman-upstream)
  if(MSVC)
    target_compile_options(chdman-upstream PRIVATE /W0)
  else()
    target_compile_options(chdman-upstream PRIVATE -w)
  endif()
  if(WIN32)
    target_link_libraries(chdman-upstream PRIVATE shlwapi)
  endif()
  if(CHDMAN_BUILD_TESTS AND TARGET chdman-cli)
    add_test(NAME parity_upstream
      COMMAND ${CMAKE_COMMAND}
        -DUPSTREAM=$<TARGET_FILE:chdman-upstream> -DCLI=$<TARGET_FILE:chdman-cli>
        -DWORK=${CMAKE_CURRENT_BINARY_DIR}/tests/parity
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/parity.cmake)
    set_tests_properties(parity_upstream PROPERTIES FIXTURES_REQUIRED chdman_tests_fixtures)
  endif()
endif()
