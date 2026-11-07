cmake_minimum_required(VERSION 3.21)
project(SC3_Launcher LANGUAGES C CXX)

# No C++ modules are used; keep GCC's module mapper out of relocatable kits with spaced/non-ASCII paths.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# SC3 Launcher.exe: what a player double-clicks. It checks their disc image, runs scripts/sc3_build.py with the build kit
# (kit\python), shows its progress, and starts the game. Its own small project (the game's CMake needs the disc): SDL3
# and Dear ImGui at the releases BozoEngine pins (its runtime/CMakeLists.txt: the same archives and SHA-256), ImGui's own
# SDL3 and OpenGL 3 backends, BozoEngine's PNG writer (the selftest's screenshots); built static, a GUI-subsystem exe
# that needs no DLL. Windows only.
#
#   cmake -S launcher -B build/launcher -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/launcher        -> build/launcher/SC3 Launcher.exe
#
# Offline (the build kit): -DFETCHCONTENT_SOURCE_DIR_SDL3=<kit>/deps/sdl3-src -DFETCHCONTENT_SOURCE_DIR_IMGUI=<kit>/deps/
# imgui-src and -DLAUNCHER_FONT_DIR=<dir with Exo2[wght].ttf and OFL.txt>.

if(NOT WIN32)
    message(FATAL_ERROR "The launcher is Windows only (Linux players run scripts/sc3_build.py).")
endif()
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# BozoEngine's tree (the submodule beside launcher/): its PNG writer only.
set(LAUNCHER_BOZO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../BozoEngine" CACHE PATH "BozoEngine's source tree")
if(NOT EXISTS "${LAUNCHER_BOZO_DIR}/runtime/app/png.cpp")
    message(FATAL_ERROR "No BozoEngine at ${LAUNCHER_BOZO_DIR} (git submodule update --init BozoEngine)")
endif()

include(FetchContent)
# SDL3, static, without audio, camera, GPU or renderer (the launcher starts only the video subsystem: mouse and
# keyboard, OpenGL).
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz
    URL_HASH SHA256=f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_MakeAvailable(SDL3)
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/b1bcb12a624af7509894c8e77dd47416997777fa.tar.gz
    URL_HASH SHA256=123926eb22d4990dea06bec0739d92f7790c8ee505b3d903fe2de7e5940f2c27
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_MakeAvailable(imgui) # no CMakeLists.txt of its own: only fetched

add_library(launcher_imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp" "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp" "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp" "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp")
target_include_directories(launcher_imgui PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
# No demo window, no debug tools.
target_compile_definitions(launcher_imgui PUBLIC IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_DEBUG_TOOLS
                                                 IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(launcher_imgui PUBLIC SDL3::SDL3-static)
target_compile_options(launcher_imgui PRIVATE -w)

# The type: Exo 2 (SIL Open Font License 1.1) from google/fonts at a pinned commit, embedded as a resource with its
# licence. Downloaded at configure time (never committed) unless LAUNCHER_FONT_DIR has the two files.
set(LAUNCHER_FONT_COMMIT "1796e34455b893092713d206e7f23e3e07699f00")
set(LAUNCHER_FONT_SHA256 "205a448676a2586f9c57c25f3d5c58ca8db7e6cf5edf7506783a010c6fe2bfb5")
set(LAUNCHER_FONT_DIR "" CACHE PATH "A folder with Exo2[wght].ttf and OFL.txt (google/fonts ofl/exo2) instead of the download")
set(_font_dir "${CMAKE_BINARY_DIR}/fonts")
if(LAUNCHER_FONT_DIR)
    file(COPY "${LAUNCHER_FONT_DIR}/Exo2[wght].ttf" "${LAUNCHER_FONT_DIR}/OFL.txt" DESTINATION "${_font_dir}")
elseif(NOT EXISTS "${_font_dir}/Exo2[wght].ttf")
    set(_raw "https://raw.githubusercontent.com/google/fonts/${LAUNCHER_FONT_COMMIT}/ofl/exo2")
    file(DOWNLOAD "${_raw}/Exo2%5Bwght%5D.ttf" "${_font_dir}/Exo2[wght].ttf"
        EXPECTED_HASH SHA256=${LAUNCHER_FONT_SHA256} STATUS _st)
    file(DOWNLOAD "${_raw}/OFL.txt" "${_font_dir}/OFL.txt")
endif()
file(SHA256 "${_font_dir}/Exo2[wght].ttf" _got)
if(NOT _got STREQUAL LAUNCHER_FONT_SHA256)
    message(FATAL_ERROR "Exo2[wght].ttf: SHA-256 ${_got}, expected ${LAUNCHER_FONT_SHA256} (google/fonts ${LAUNCHER_FONT_COMMIT})")
endif()
# windres does not like brackets in names.
configure_file("${_font_dir}/Exo2[wght].ttf" "${_font_dir}/exo2.ttf" COPYONLY)

# The icon: The Bozos' logo (TMB_Recomp's docs/branding/bozos_icon_1024.png), as a multi-size .ico (16 to 256 px).
set(_icon "${CMAKE_CURRENT_SOURCE_DIR}/sc3.ico")

# The release's version and source commit (scripts/make_release.py passes them): shown in the footer, compared with
# GitHub's latest release by the update check, in the exe's version information. "dev" builds skip the check at start.
set(LAUNCHER_VERSION "dev" CACHE STRING "The release version (scripts/make_release.py: the tag or --version)")
set(LAUNCHER_COMMIT "" CACHE STRING "The SoulCaliburIII_Recomp commit the release is made from")
if(NOT LAUNCHER_VERSION MATCHES "^[A-Za-z0-9._+-]+$" OR NOT LAUNCHER_COMMIT MATCHES "^[0-9a-f]*$")
    message(FATAL_ERROR "LAUNCHER_VERSION / LAUNCHER_COMMIT: unexpected characters")
endif()
set(LAUNCHER_VERSION_NUMBERS 0 0 0)
if(LAUNCHER_VERSION MATCHES "^v?([0-9]+)[.]([0-9]+)[.]([0-9]+)")
    set(LAUNCHER_VERSION_NUMBERS ${CMAKE_MATCH_1} ${CMAKE_MATCH_2} ${CMAKE_MATCH_3})
endif()
list(JOIN LAUNCHER_VERSION_NUMBERS "," LAUNCHER_FILEVERSION)

set(LAUNCHER_ICON "${_icon}")
set(LAUNCHER_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/launcher.manifest")
set(LAUNCHER_EXO2 "${_font_dir}/exo2.ttf")
set(LAUNCHER_OFL "${_font_dir}/OFL.txt")
configure_file(launcher.rc.in "${CMAKE_BINARY_DIR}/launcher.rc" @ONLY)
set_source_files_properties("${CMAKE_BINARY_DIR}/launcher.rc" PROPERTIES OBJECT_DEPENDS "${_icon};${LAUNCHER_EXO2};${LAUNCHER_MANIFEST}")
enable_language(RC)

add_executable(sc3_launcher WIN32
    src/main.cpp src/ui.cpp src/window.cpp src/build_model.cpp src/config.cpp src/platform_win.cpp
    "${LAUNCHER_BOZO_DIR}/runtime/app/png.cpp"
    "${CMAKE_BINARY_DIR}/launcher.rc")
set_target_properties(sc3_launcher PROPERTIES OUTPUT_NAME "SC3 Launcher")
target_include_directories(sc3_launcher PRIVATE "${LAUNCHER_BOZO_DIR}/runtime/include")
target_link_libraries(sc3_launcher PRIVATE launcher_imgui comdlg32 shell32 ole32 winhttp)
target_compile_definitions(sc3_launcher PRIVATE SC3_VERSION="${LAUNCHER_VERSION}" SC3_COMMIT="${LAUNCHER_COMMIT}")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # No DLL besides Windows' own: libstdc++, libgcc and winpthread linked in; no debug information.
    target_link_options(sc3_launcher PRIVATE -static -s)
endif()
