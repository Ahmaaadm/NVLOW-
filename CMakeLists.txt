cmake_minimum_required(VERSION 3.20)

project(NVLOW
    VERSION 1.1.0
    DESCRIPTION "Shrink NVIDIA recordings without losing quality"
    LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# GitHub repository ("owner/name") whose Releases NVLOW checks for updates.
set(NVLOW_GITHUB_REPO "Ahmaaadm/NVLOW" CACHE STRING "GitHub repo used for auto-updates")

add_executable(NVLOW WIN32
    src/main.cpp
    src/MainWindow.cpp
    src/Encoder.cpp
    src/MediaProbe.cpp
    src/Settings.cpp
    src/Updater.cpp
    src/Utils.cpp
    src/VideoScanner.cpp
    res/nvlow.rc
)

target_compile_definitions(NVLOW PRIVATE
    UNICODE _UNICODE NOMINMAX
    _WIN32_WINNT=0x0A00 WINVER=0x0A00
    NVLOW_VERSION=L"${PROJECT_VERSION}"
    NVLOW_GITHUB_REPO=L"${NVLOW_GITHUB_REPO}")

target_link_libraries(NVLOW PRIVATE
    comctl32 shell32 shlwapi ole32 uuid uxtheme dwmapi
    mfplat mfreadwrite mfuuid d3d11 dxgi winhttp bcrypt)

if(MSVC)
    target_compile_options(NVLOW PRIVATE /W4 /utf-8 /permissive-)
    # The manifest is embedded through res/nvlow.rc.
    target_link_options(NVLOW PRIVATE /MANIFEST:NO)
    set_property(TARGET NVLOW PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(NVLOW PRIVATE -Wall -Wextra -Wno-missing-field-initializers -finput-charset=UTF-8 -fexec-charset=UTF-8)
    # MinGW links its own default-manifest.o, which clashes with ours (res/nvlow.manifest).
    # Shadow it with an empty object found first via -B.
    set(NOMANIFEST_DIR "${CMAKE_BINARY_DIR}/nomanifest")
    file(MAKE_DIRECTORY "${NOMANIFEST_DIR}")
    file(WRITE "${NOMANIFEST_DIR}/empty.cpp" "")
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -c "${NOMANIFEST_DIR}/empty.cpp"
                            -o "${NOMANIFEST_DIR}/default-manifest.o")
    # Static runtime so NVLOW.exe runs on any machine without MinGW DLLs.
    # -B is relative (the linker runs in the build dir) because GCC specs break on paths with spaces.
    file(RELATIVE_PATH NOMANIFEST_REL "${CMAKE_CURRENT_BINARY_DIR}" "${NOMANIFEST_DIR}")
    target_link_options(NVLOW PRIVATE -municode -static -s "-B${NOMANIFEST_REL}/")
endif()
