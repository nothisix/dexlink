cmake_minimum_required(VERSION 3.20)
project(dexlink_pc_player LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
pkg_check_modules(SDL2_PKG REQUIRED IMPORTED_TARGET sdl2)

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
set(JSON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include")

add_executable(dexlink
    src/main.cpp
    src/dexlink.rc
    src/adb/AdbManager.cpp
    src/net/UsbTransport.cpp
    src/video/D3D11Renderer.cpp
    src/video/VideoDecoder.cpp
    src/audio/WasapiAudioPlayer.cpp
    src/input/Keymapper.cpp
    src/ui/DexlinkUi.cpp
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
)

target_include_directories(dexlink PRIVATE
    src
    "${CMAKE_CURRENT_SOURCE_DIR}/../protocol/cpp"
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
    "${JSON_DIR}"
)

target_compile_definitions(dexlink PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    SDL_MAIN_HANDLED
    IMGUI_DEFINE_MATH_OPERATORS
)

target_compile_options(dexlink PRIVATE
    -Wall
    -Wextra
    -Wno-missing-field-initializers
)

target_link_libraries(dexlink PRIVATE
    PkgConfig::FFMPEG
    PkgConfig::SDL2_PKG
    d3d11
    dxgi
    d3dcompiler
    ws2_32
    ole32
    avrt
    dwmapi
)

# Copy runtime assets (server jar if built, profiles) next to binary for turnkey launch
add_custom_command(TARGET dexlink POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/../profiles"
        "$<TARGET_FILE_DIR:dexlink>/profiles"
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../android-server/build/dexlink-server.jar")
    add_custom_command(TARGET dexlink POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../android-server/build/dexlink-server.jar"
            "$<TARGET_FILE_DIR:dexlink>/dexlink-server.jar"
    )
endif()

add_executable(dexlink_core_test
    tests/frame_keymapper_test.cpp
    src/input/Keymapper.cpp
    src/net/UsbTransport.cpp
    src/video/D3D11Renderer.cpp
)

target_include_directories(dexlink_core_test PRIVATE
    src
    "${CMAKE_CURRENT_SOURCE_DIR}/../protocol/cpp"
    "${JSON_DIR}"
)

target_compile_definitions(dexlink_core_test PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    SDL_MAIN_HANDLED
)

target_link_libraries(dexlink_core_test PRIVATE
    PkgConfig::FFMPEG
    PkgConfig::SDL2_PKG
    d3d11
    dxgi
    ws2_32
)

target_link_options(dexlink_core_test PRIVATE -Wl,--subsystem,console)
