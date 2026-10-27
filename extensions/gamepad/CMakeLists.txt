cmake_minimum_required(VERSION 3.25)
project(CemuRemoteGamePad VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The first GamePad backend requires Windows x64.")
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(Boost 1.89 CONFIG REQUIRED COMPONENTS filesystem)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Opus CONFIG REQUIRED)
find_path(NVENC_INCLUDE_DIR nvEncodeAPI.h PATH_SUFFIXES ffnvcodec REQUIRED)

set(CORE "${CMAKE_CURRENT_SOURCE_DIR}/host-core/protocol")
set(MOONLIGHT "${CMAKE_CURRENT_SOURCE_DIR}/host-core/upstream/third-party/moonlight-common-c")
set(WEB "${CMAKE_CURRENT_SOURCE_DIR}/host-core/upstream/third-party/Simple-Web-Server")
if(NOT EXISTS "${MOONLIGHT}/enet/CMakeLists.txt" OR NOT EXISTS "${CORE}/src/stream.cpp")
    message(FATAL_ERROR "Run packaging/prepare-sources.ps1 before configuring the plugin.")
endif()
set(ENET_NO_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory("${MOONLIGHT}/enet" "${CMAKE_CURRENT_BINARY_DIR}/enet" EXCLUDE_FROM_ALL)

add_library(CemuGamePad SHARED
    plugin/plugin.cpp plugin/runtime.cpp plugin/nvenc.cpp
    host-core/input.cpp host-core/media.cpp host-core/platform.cpp host-core/control.cpp
    "${CORE}/src/crypto.cpp" "${CORE}/src/network.cpp"
    "${CORE}/src/nvhttp.cpp" "${CORE}/src/rtsp.cpp" "${CORE}/src/stream.cpp"
    "${CORE}/src/file_handler.cpp"
    "${MOONLIGHT}/src/RtspParser.c"
    "${MOONLIGHT}/nanors/rs.c"
    "${MOONLIGHT}/nanors/deps/obl/oblas_common.c"
    "${MOONLIGHT}/nanors/deps/obl/oblas_lite.c")
target_include_directories(CemuGamePad PRIVATE
    sdk plugin host-core "${CORE}" "${CORE}/src"
    "${MOONLIGHT}/.." "${MOONLIGHT}/enet/include" "${MOONLIGHT}/nanors"
    "${MOONLIGHT}/nanors/deps/obl" "${WEB}/.." "${NVENC_INCLUDE_DIR}")
target_compile_definitions(CemuGamePad PRIVATE CGP_BUILD_PLUGIN WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00 BOOST_BIND_GLOBAL_PLACEHOLDERS SUNSHINE_TRAY=0)
target_link_libraries(CemuGamePad PRIVATE Boost::headers Boost::filesystem OpenSSL::SSL OpenSSL::Crypto
    nlohmann_json::nlohmann_json Opus::opus enet d3d11 dxgi ws2_32 iphlpapi avrt qwave advapi32)
if(MSVC)
    target_compile_options(CemuGamePad PRIVATE /EHsc /W4 /permissive-)
endif()
add_executable(gamepadctl manager/main.cpp)
target_compile_features(gamepadctl PRIVATE cxx_std_20)
target_link_libraries(gamepadctl PRIVATE nlohmann_json::nlohmann_json advapi32)
target_compile_definitions(gamepadctl PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
install(TARGETS CemuGamePad gamepadctl RUNTIME DESTINATION extensions/gamepad)
install(FILES packaging/config.example.json DESTINATION extensions/gamepad)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/host-core/upstream/LICENSE"
    DESTINATION extensions/gamepad/licenses RENAME Sunshine-LICENSE)
install(FILES host-core/upstream.lock.json DESTINATION extensions/gamepad)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../LICENSE.txt"
    DESTINATION extensions/gamepad/licenses RENAME Cemu-MPL-2.0-LICENSE)
install(FILES compat/validated.json compat/candidate.json DESTINATION extensions/gamepad/compat)
install(FILES README.zh-CN.md BUILD-RESULT.zh-CN.md sdk/ABI.zh-CN.md host-core/README.md
    DESTINATION extensions/gamepad/docs)
install(FILES LICENSING.zh-CN.md THIRD_PARTY_NOTICES.md DESTINATION extensions/gamepad/docs)
install(FILES LICENSE.txt DESTINATION extensions/gamepad/licenses RENAME GamePad-GPL-3.0-LICENSE)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../LICENSE-NOTICE.md" DESTINATION extensions/gamepad/licenses)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../LICENSE-NOTICE.md" "${CMAKE_CURRENT_SOURCE_DIR}/../../LICENSE.txt"
    DESTINATION .)
install(DIRECTORY licenses/ DESTINATION extensions/gamepad/licenses/third-party)
install(FILES packaging/summarize-timeline.py DESTINATION extensions/gamepad/tools)
install(FILES "${MOONLIGHT}/LICENSE.txt" DESTINATION extensions/gamepad/licenses RENAME Moonlight-common-c-LICENSE)
install(FILES "${MOONLIGHT}/enet/LICENSE" DESTINATION extensions/gamepad/licenses RENAME ENet-LICENSE)
install(FILES "${MOONLIGHT}/nanors/LICENSE" DESTINATION extensions/gamepad/licenses RENAME nanors-LICENSE)
install(FILES "${WEB}/LICENSE" DESTINATION extensions/gamepad/licenses RENAME Simple-Web-Server-LICENSE)
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    install(DIRECTORY "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/"
        DESTINATION extensions/gamepad/licenses/vcpkg FILES_MATCHING PATTERN "copyright")
endif()
