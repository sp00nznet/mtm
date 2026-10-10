# =============================================================================
# Monster Truck Madness 2 - static recompilation host (32-bit, MSVC x86)
#
#   py -3 run_lift.py        # -> src/recomp/gen/  (needs work/functions_monster.json)
#   build.cmd                # vcvarsall amd64_x86 + CMake + Ninja -> build/mtm.exe
#   build\mtm.exe --run
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(mtm C)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "the host must be 32-bit: use build.cmd (vcvarsall amd64_x86)")
endif()
if(NOT MSVC)
  message(FATAL_ERROR "runtime/native32 is MSVC x86 inline assembly")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(PCRECOMP "${CMAKE_SOURCE_DIR}/../pcrecomp" CACHE PATH "pcrecomp checkout")
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/recomp/gen/*.c")
if(NOT GEN_SOURCES)
  message(FATAL_ERROR "src/recomp/gen is empty -- run `py -3 run_lift.py` first")
endif()

set(HOST_SOURCES src/runtime/host.c src/runtime/ddraw.c src/runtime/present.c)
add_executable(mtm
  ${HOST_SOURCES}
  ${PCRECOMP}/runtime/native32/native32.c
  ${PCRECOMP}/runtime/recomp32/image_loader.c
  ${PCRECOMP}/runtime/recomp32/recomp_trace.c
  ${GEN_SOURCES}
)
target_include_directories(mtm PRIVATE
  src/runtime src/recomp/gen ${PCRECOMP}/runtime/native32 ${PCRECOMP}/runtime/recomp32)
# RECOMP_RETADDR=0u: a stack imbalance during bring-up leaks a harmless 0, not
# 0xDEAD0000 into a size argument (forcecommander).
target_compile_definitions(mtm PRIVATE RECOMP_RETADDR=0u _CRT_SECURE_NO_WARNINGS
                                                      RECOMP_FLAT_MEMORY RECOMP_LOCAL_REGS)
# Linked high and not relocatable, so Monster.exe's 0x00400000 is free for the
# guest image.
target_link_options(mtm PRIVATE /BASE:0x60000000 /FIXED:NO /DYNAMICBASE:NO
                                               /NXCOMPAT /STACK:0x1000000 /MANIFESTUAC:NO)
set(GEN_OPT "/O2" CACHE STRING "optimisation for the generated C")
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/w;${GEN_OPT}")
set_source_files_properties(${HOST_SOURCES} PROPERTIES COMPILE_OPTIONS "/W3")
target_link_libraries(mtm PRIVATE user32 gdi32 kernel32 advapi32 winmm dwmapi d3d11 dxgi dxguid)

option(MTM_TRACE "record a ring buffer of lifted function entries" OFF)
if(MTM_TRACE)
  target_compile_definitions(mtm PRIVATE RECOMP_TRACE=1)
endif()
