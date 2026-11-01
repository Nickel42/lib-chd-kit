# Parity test: run the same commands through unmodified upstream chdman and
# through chdman-cli (lib-chd-kit) and require byte-identical outputs.
# Inputs are the fixtures written by chdman_tests into tests/work_shared.
#
#   cmake -DUPSTREAM=<chdman-upstream> -DCLI=<chdman-cli> -DWORK=<dir> -P parity.cmake

get_filename_component(FIX "${WORK}/../work_shared" ABSOLUTE)
if(NOT EXISTS "${FIX}/raw.img")
  message(FATAL_ERROR "fixtures missing in ${FIX}; run chdman_tests first")
endif()
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/up" "${WORK}/lib")

set(failures 0)

function(run tool tag)
  if(tool STREQUAL "up")
    set(exe "${UPSTREAM}")
  else()
    set(exe "${CLI}")
  endif()
  string(REPLACE "@D@" "${WORK}/${tool}" args "${ARGN}")
  execute_process(COMMAND "${exe}" ${args}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(SEND_ERROR "[${tag}] ${tool} failed (${rc}): ${err}")
  endif()
endfunction()

function(same tag relpath)
  file(SHA256 "${WORK}/up/${relpath}" a)
  file(SHA256 "${WORK}/lib/${relpath}" b)
  if(a STREQUAL b)
    message(STATUS "[${tag}] ${relpath} identical")
  else()
    message(SEND_ERROR "[${tag}] ${relpath} differs between upstream chdman and lib-chd-kit")
  endif()
endfunction()

foreach(t up lib)
  run(${t} createraw createraw -i "${FIX}/raw.img" -o "@D@/raw.chd" -us 512 -f)
  run(${t} createraw_hs createraw -i "${FIX}/raw.img" -o "@D@/raw_hs.chd" -us 512 -hs 16384 -c lzma,zstd -f)
  run(${t} createhd createhd -i "${FIX}/hd.img" -o "@D@/hd.chd" -chs 4,16,63 -f)
  run(${t} createhd_blank createhd -o "@D@/blank.chd" -tp 3 -f)
  run(${t} createcd createcd -i "${FIX}/disc.cue" -o "@D@/disc.chd" -f)
  run(${t} createdvd createdvd -i "${FIX}/dvd.iso" -o "@D@/dvd.chd" -f)
  run(${t} createld createld -i "${FIX}/ld.avi" -o "@D@/ld.chd" -f)
  run(${t} copy copy -i "@D@/raw.chd" -o "@D@/copy.chd" -c zstd,huff -hs 8192 -f)
  run(${t} copy_cd copy -i "@D@/disc.chd" -o "@D@/disc_copy.chd" -f)
  run(${t} extractcd extractcd -i "@D@/disc.chd" -o "@D@/out.cue" -f)
  run(${t} extractcd_toc extractcd -i "@D@/disc.chd" -o "@D@/out.toc" -f)
  run(${t} extractcd_gdi extractcd -i "@D@/disc.chd" -o "@D@/out.gdi" -f)
  run(${t} extractraw extractraw -i "@D@/raw.chd" -o "@D@/raw.out" -isb 4096 -ib 100000 -f)
  run(${t} extractld extractld -i "@D@/ld.chd" -o "@D@/ld.avi" -f)
endforeach()

foreach(f raw.chd raw_hs.chd hd.chd blank.chd disc.chd dvd.chd ld.chd copy.chd disc_copy.chd
          out.cue out.bin out.toc out.gdi raw.out ld.avi)
  same(parity ${f})
endforeach()
# GDI output writes one file per track
foreach(f out01.bin out02.raw)
  same(parity ${f})
endforeach()
