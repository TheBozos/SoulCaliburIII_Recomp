# SC3's sound modules (bozo/sound/sc3_audio.h): CRI_ADXI, NUSOUND and SDRDRV translated at build time from the player's disc
# by BozoEngine's tools/native_irx. Everything generated stays in the build tree. Included by bozo.cmake once its programs exist.
option(SC3_NATIVE_SOUND "SC3's sound modules translated and run natively (BozoEngine needs sc3/irx-sound: on once the submodule's pin has it)" OFF)
if(NOT SC3_NATIVE_SOUND)
    return()
endif()
set(SC3_IRX_DIR "" CACHE PATH "The disc's IRX folder (default: next to SC3_ELF)")
if(NOT SC3_IRX_DIR)
    get_filename_component(sc3_disc "${SC3_ELF}" DIRECTORY)
    set(SC3_IRX_DIR "${sc3_disc}/IRX")
endif()
foreach(m CRI_ADXI NUSOUND SDRDRV)
    if(NOT EXISTS "${SC3_IRX_DIR}/${m}.IRX")
        message(FATAL_ERROR "SC3: ${SC3_IRX_DIR}/${m}.IRX not found (set SC3_IRX_DIR to the disc's IRX folder)")
    endif()
endforeach()

set(irx_dir "${gen_dir}/iop")
set(native_irx "${BOZO_ENGINE_DIR}/tools/native_irx")
set(irx_sources)
set(irx_images)
foreach(spec "CRI_ADXI:cri_adxi:0x8000" "NUSOUND:nusound:0x1f000" "SDRDRV:sdrdrv:0x25000")
    string(REPLACE ":" ";" spec "${spec}")
    list(GET spec 0 module)
    list(GET spec 1 stem)
    list(GET spec 2 base)
    add_custom_command(
        OUTPUT ${irx_dir}/${stem}.json ${irx_dir}/${stem}.bin ${irx_dir}/${stem}_iop.cpp
        COMMAND ${CMAKE_COMMAND} -E make_directory ${irx_dir}
        COMMAND Python3::Interpreter -I "${native_irx}/relocate.py" "${SC3_IRX_DIR}/${module}.IRX" ${base} ${irx_dir}/${stem}
        COMMAND Python3::Interpreter -I "${native_irx}/recompile.py" ${irx_dir}/${stem}.json --out ${irx_dir}
        DEPENDS "${SC3_IRX_DIR}/${module}.IRX" "${native_irx}/relocate.py" "${native_irx}/recompile.py"
        COMMENT "Compiling ${module}.IRX")
    list(APPEND irx_sources ${irx_dir}/${stem}_iop.cpp)
    list(APPEND irx_images ${irx_dir}/${stem}.json)
endforeach()
add_custom_command(
    OUTPUT ${irx_dir}/sc3_irx_images.cpp
    COMMAND Python3::Interpreter -I "${CMAKE_SOURCE_DIR}/scripts/sc3_irx_images.py" "${native_irx}" ${irx_dir}/sc3_irx_images.cpp ${irx_images}
    DEPENDS "${CMAKE_SOURCE_DIR}/scripts/sc3_irx_images.py" ${irx_sources}
    COMMENT "Writing SC3's IOP module images")
add_library(sc3_iop OBJECT ${irx_sources} ${irx_dir}/sc3_irx_images.cpp)
set(sound_dir "${CMAKE_SOURCE_DIR}/bozo/sound")
target_include_directories(sc3_iop PRIVATE "${sound_dir}")
target_link_libraries(sc3_iop PUBLIC bozo_port bozo_libs)
target_compile_options(sc3_iop PRIVATE -w)
foreach(program sc3-headless sc3-check sc3)
    if(TARGET ${program})
        target_sources(${program} PRIVATE "${sound_dir}/sc3_audio_modules.cpp")
        target_include_directories(${program} PRIVATE "${sound_dir}")
        target_compile_definitions(${program} PRIVATE SC3_NATIVE_SOUND)
        target_link_libraries(${program} PRIVATE sc3_iop)
    endif()
endforeach()

# The modules' start alone, with no game: needs none of the EE's generated code.
add_executable(sc3-irx-check "${sound_dir}/sc3_irx_check.cpp" "${sound_dir}/sc3_audio_modules.cpp")
target_include_directories(sc3-irx-check PRIVATE "${sound_dir}")
target_link_libraries(sc3-irx-check PRIVATE sc3_iop bozo_port bozo_libs bozo_runtime_core)
