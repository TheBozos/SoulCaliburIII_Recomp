# SC3 on BozoEngine, next to the PS2Recomp runner until the switch (README.md, "On BozoEngine"). Selected by SC3_BOZO=ON:
#
#   cmake -S . -B build/bozo -G Ninja -DSC3_BOZO=ON -DSC3_ELF=<SLUS_212.16> [-DSC3_ISO=<the disc image>]
#         [-DBOZO_ENGINE_DIR=<BozoEngine>] [-DSC3_SYMBOL_MAP=<sc3_symbols.txt>] [-DSC3_WINDOW=OFF]
#
# Targets:
# - sc3-irx-check: SC3's sound modules started natively with no game (SC3_NATIVE_SOUND, cmake/sc3_audio.cmake).
# - sc3: the game in a window, in real time (BozoEngine's bozo_app: SDL3, OpenGL 3.3; SC3_WINDOW, on by default).
# - sc3-headless: the game on BozoEngine's free host with no window (every library native, BozoEngine's own timing).
# - sc3-check: BozoEngine's call-log checker (bozo-run) with SC3's overrides running natively:
#   sc3-check <log.clg> --pinned --ref <run.ref> ...
# - sc3_recompile: the generated code alone.
#
# The generated code: bozo-recomp with the externals bozo-externals writes from the ELF (the names BozoEngine answers),
# recomp/sc3_stubs.txt, SC3's override list (scripts/sc3_overrides_list.py, from bozo/port/*.cpp) and the symbol map
# SC3_SYMBOL_MAP (default recomp/sc3_symbols.txt, once BOZO_ENGINE_DIR's engine reads symbol maps).

set(BOZO_ENGINE_DIR "" CACHE PATH "BozoEngine's source tree (default: the BozoEngine submodule)")
if(NOT BOZO_ENGINE_DIR AND EXISTS "${CMAKE_SOURCE_DIR}/BozoEngine/CMakeLists.txt")
    set(BOZO_ENGINE_DIR "${CMAKE_SOURCE_DIR}/BozoEngine" CACHE PATH "BozoEngine's source tree (default: the BozoEngine submodule)" FORCE)
endif()
set(SC3_ELF "" CACHE FILEPATH "SC3's executable (SLUS_212.16), from the disc")
if(NOT SC3_ELF AND EXISTS "${CMAKE_SOURCE_DIR}/game/SLUS_212.16")
    set(SC3_ELF "${CMAKE_SOURCE_DIR}/game/SLUS_212.16" CACHE FILEPATH "SC3's executable (SLUS_212.16), from the disc" FORCE)
endif()
set(SC3_ISO "" CACHE FILEPATH "SC3's disc image, the default of the programs' --iso")
set(SC3_SYMBOL_MAP "" CACHE FILEPATH "Names of the stripped ELF's library functions (name 0xADDR lines), given to bozo-externals and bozo-recomp --symbols and to the free host")
set(SC3_EXTERNALS "" CACHE FILEPATH "An externals list to generate with instead of bozo-externals' (bozo-calllog-tool externals: a pinned check needs the reference's stubs)")
set(SC3_ENTRIES "${CMAKE_SOURCE_DIR}/recomp/sc3_entries.txt" CACHE FILEPATH "Explicit entry points for bozo-recomp --entries")
set(SC3_GEN_FILES 32 CACHE STRING "How many .cpp files the generated code is split into")
set(SC3_GEN_OPT "-O1" CACHE STRING "Optimisation of the generated code")
option(SC3_WINDOW "Build sc3, the game in a window (BozoEngine's bozo_app: SDL3, OpenGL 3.3)" ON)
if(NOT BOZO_ENGINE_DIR OR NOT EXISTS "${BOZO_ENGINE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "SC3: set BOZO_ENGINE_DIR to BozoEngine's source tree (or git submodule update --init BozoEngine)")
endif()
if(NOT SC3_ELF OR NOT EXISTS "${SC3_ELF}")
    message(FATAL_ERROR "SC3: set SC3_ELF to the game's executable (SLUS_212.16)")
endif()
if(NOT SC3_SYMBOL_MAP)
    file(STRINGS "${BOZO_ENGINE_DIR}/tools/externals/main.cpp" engine_reads_symbol_maps REGEX "\"--symbols\"")
    if(engine_reads_symbol_maps)
        set(SC3_SYMBOL_MAP "${CMAKE_SOURCE_DIR}/recomp/sc3_symbols.txt" CACHE FILEPATH "Names of the stripped ELF's library functions (name 0xADDR lines), given to bozo-externals and bozo-recomp --symbols and to the free host" FORCE)
    endif()
endif()
if(SC3_SYMBOL_MAP AND NOT EXISTS "${SC3_SYMBOL_MAP}")
    message(FATAL_ERROR "SC3: SC3_SYMBOL_MAP ${SC3_SYMBOL_MAP} does not exist")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(BOZO_TMB_ELF "" CACHE FILEPATH "" FORCE)
set(BOZO_APP ${SC3_WINDOW} CACHE BOOL "" FORCE)
add_subdirectory("${BOZO_ENGINE_DIR}" bozo)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(gen_dir "${CMAKE_BINARY_DIR}/sc3_gen")
set(overrides_list "${gen_dir}/sc3_overrides.txt")
set(externals "${gen_dir}/sc3_externals.txt")
set(port_dir "${CMAKE_SOURCE_DIR}/bozo/port")
file(GLOB SC3_PORT_SOURCES CONFIGURE_DEPENDS "${port_dir}/*.cpp")

add_custom_command(
    OUTPUT ${overrides_list}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${gen_dir}
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/sc3_overrides_list.py" "${port_dir}" ${overrides_list}
    DEPENDS "${CMAKE_SOURCE_DIR}/scripts/sc3_overrides_list.py" ${SC3_PORT_SOURCES}
    COMMENT "Listing SC3's overrides")
set(externals_args --overrides ${overrides_list} --stubs "${CMAKE_SOURCE_DIR}/recomp/sc3_stubs.txt")
set(externals_depends ${overrides_list} "${CMAKE_SOURCE_DIR}/recomp/sc3_stubs.txt")
if(SC3_SYMBOL_MAP)
    list(APPEND externals_args --symbols "${SC3_SYMBOL_MAP}")
    list(APPEND externals_depends "${SC3_SYMBOL_MAP}")
endif()
if(SC3_EXTERNALS)
    set(externals "${SC3_EXTERNALS}")
else()
    add_custom_command(
        OUTPUT ${externals}
        COMMAND bozo-externals ${SC3_ELF} ${externals} ${externals_args}
        DEPENDS bozo-externals ${externals_depends} ${SC3_ELF}
        COMMENT "Writing the externals list")
endif()

set(gen_files ${gen_dir}/sc3_functions.h ${gen_dir}/sc3_table.cpp)
math(EXPR last "${SC3_GEN_FILES} - 1")
foreach(i RANGE ${last})
    string(LENGTH "${i}" len)
    if(len EQUAL 1)
        set(n "00${i}")
    elseif(len EQUAL 2)
        set(n "0${i}")
    else()
        set(n "${i}")
    endif()
    list(APPEND gen_files ${gen_dir}/sc3_${n}.cpp)
endforeach()
set(sc3_patches "${CMAKE_SOURCE_DIR}/recomp/sc3_patches.txt")
set(recomp_args --prefix sc3 --files ${SC3_GEN_FILES} --no-comments --externals ${externals} --patches ${sc3_patches})
set(recomp_depends bozo-recomp ${externals} ${SC3_ELF} ${sc3_patches})
if(SC3_SYMBOL_MAP)
    list(APPEND recomp_args --symbols "${SC3_SYMBOL_MAP}")
    list(APPEND recomp_depends "${SC3_SYMBOL_MAP}")
endif()
if(SC3_ENTRIES)
    list(APPEND recomp_args --entries "${SC3_ENTRIES}")
    list(APPEND recomp_depends "${SC3_ENTRIES}")
endif()
add_custom_command(
    OUTPUT ${gen_files}
    COMMAND bozo-recomp ${SC3_ELF} ${gen_dir} ${recomp_args}
    DEPENDS ${recomp_depends}
    COMMENT "Recompiling SC3")
add_custom_target(sc3_recompile DEPENDS ${gen_files})
add_library(sc3_generated OBJECT ${gen_files})
target_link_libraries(sc3_generated PUBLIC bozo_runtime_headers)
target_include_directories(sc3_generated PUBLIC ${gen_dir})
target_compile_options(sc3_generated PRIVATE ${SC3_GEN_OPT} -w)

# An object library: BOZO_PORT's registration must be linked whatever references it.
add_library(sc3_port OBJECT ${SC3_PORT_SOURCES})
target_include_directories(sc3_port PUBLIC "${port_dir}" ${gen_dir})
target_link_libraries(sc3_port PUBLIC bozo_port bozo_render bozo_libs bozo_runtime_core)
add_dependencies(sc3_port sc3_generated)
if(BOZO_FFMPEG_ROOT)
    foreach(lib avformat swresample swscale)
        find_library(SC3_${lib} ${lib} PATHS ${BOZO_FFMPEG_ROOT}/lib NO_DEFAULT_PATH)
    endforeach()
    if(SC3_avformat AND SC3_swresample AND SC3_swscale)
        target_include_directories(sc3_port PRIVATE ${BOZO_FFMPEG_ROOT}/include)
        target_link_libraries(sc3_port PUBLIC ${SC3_avformat} ${BOZO_AVCODEC} ${SC3_swresample} ${SC3_swscale} ${BOZO_AVUTIL})
        target_compile_definitions(sc3_port PRIVATE SC3_HAVE_FFMPEG)
    endif()
else()
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(SC3_FFMPEG IMPORTED_TARGET libavformat libavcodec libswresample libswscale libavutil)
        if(SC3_FFMPEG_FOUND)
            target_link_libraries(sc3_port PUBLIC PkgConfig::SC3_FFMPEG)
            target_compile_definitions(sc3_port PRIVATE SC3_HAVE_FFMPEG)
        endif()
    endif()
endif()
target_compile_options(sc3_port PRIVATE -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable)

set(sc3_main_defines BOZO_RUN_DEFAULT_ELF="${SC3_ELF}" SC3_DEFAULT_ISO="${SC3_ISO}" SC3_DEFAULT_SYMBOL_MAP="${SC3_SYMBOL_MAP}")

function(sc3_static_link target)
    if(MINGW)
        target_link_options(${target} PRIVATE -static -Wl,--stack,268435456)
    endif()
endfunction()

add_executable(sc3-headless bozo/sc3_headless.cpp)
target_link_libraries(sc3-headless PRIVATE sc3_port sc3_generated bozo_host)
target_compile_definitions(sc3-headless PRIVATE ${sc3_main_defines})
sc3_static_link(sc3-headless)

# bozo-run's library names the generated tables tmb_*: they are SC3's here.
if(TARGET bozo_run)
    add_executable(sc3-check bozo/sc3_check.cpp)
    target_link_libraries(sc3-check PRIVATE sc3_port sc3_generated bozo_run)
    target_compile_definitions(sc3-check PRIVATE ${sc3_main_defines})
    foreach(table function_table function_count resume_table resume_count external_table external_count)
        target_link_options(sc3-check PRIVATE "LINKER:--defsym=tmb_${table}=sc3_${table}")
    endforeach()
    sc3_static_link(sc3-check)
endif()

if(SC3_WINDOW)
    add_executable(sc3 bozo/sc3_window.cpp)
    target_link_libraries(sc3 PRIVATE sc3_port sc3_generated bozo_host bozo_app)
    target_compile_definitions(sc3 PRIVATE ${sc3_main_defines})
    sc3_static_link(sc3)
endif()

include("${CMAKE_SOURCE_DIR}/cmake/sc3_audio.cmake")
