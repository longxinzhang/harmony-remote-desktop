cmake_minimum_required(VERSION 3.5.0)
project(hrd_probe)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_library(hrd_probe SHARED napi_init.cpp capture_probe.cpp input_probe.cpp remote_input.cpp encoder_probe.cpp lan_server.cpp clipboard_service.cpp clipboard_platform.cpp pairing_identity.cpp audio_service.cpp)
target_compile_options(hrd_probe PRIVATE -Wall -Wextra -Werror=return-type)
target_link_libraries(hrd_probe PUBLIC libace_napi.z.so libhilog_ndk.z.so
  libnative_avscreen_capture.so libnative_media_core.so libnative_buffer.so
  libnative_display_manager.so libohinput.so libnative_media_venc.so
  libnative_media_codecbase.so libnative_window.so libpasteboard.so libudmf.so libohcrypto.so libhuks_ndk.z.so)
