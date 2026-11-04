cmake_minimum_required(VERSION 3.20)
if(WIN32)
  project(MultiplayerEverythingLauncher VERSION 1.0.43 LANGUAGES CXX RC)
else()
  # Linux: the engine alone (no window), for the app's mod commands and lobby; docs/LINUX.md.
  project(MultiplayerEverythingLauncher VERSION 1.0.43 LANGUAGES CXX ASM)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the launcher is a single self-contained .exe.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The C++ launcher, kept in old/ while the Tauri app (at the top of the repository) takes over its work: the games, covers and
# launcher.json it embeds are shared with the new app and stay at the top of the repository.
get_filename_component(REPO_ROOT "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)

set(LAUNCHER_NAME "MultiplayerEverythingLauncher" CACHE STRING "Output name of the launcher executable")
set(LAUNCHER_ICON "${CMAKE_SOURCE_DIR}/assets/app.ico" CACHE FILEPATH "Executable icon")

add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
if(MSVC)
  add_compile_options(/W4 /utf-8 /permissive- /EHsc /wd4100)
elseif(NOT WIN32)
  add_compile_options(-Wall -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function)
endif()

# ---- core: no UI, shared by the launcher and the tests --------------------
add_library(launcher_core STATIC
  src/core/config.cpp
  src/core/discord.cpp
  src/core/embedded.cpp
  src/core/fs.cpp
  src/core/game_options.cpp
  src/core/install.cpp
  src/core/json.cpp
  src/core/library.cpp
  src/core/mod/detect.cpp
  src/core/mod/engine.cpp
  src/core/mod/manager.cpp
  src/core/mod/package.cpp
  src/core/mod/resources.cpp
  src/core/mod/spec.cpp
  src/core/mod/state.cpp
  src/core/log.cpp
  src/core/motion.cpp
  src/core/process.cpp
  src/core/lobby.cpp
  src/core/net.cpp
  src/core/relay.cpp
  src/core/rooms.cpp
  src/core/site.cpp
  src/core/community.cpp
  src/core/guide.cpp
  src/core/settings.cpp
  src/core/strings.cpp
  src/core/vars.cpp
  src/core/webserver.cpp
)
target_include_directories(launcher_core PUBLIC src)
target_compile_definitions(launcher_core PUBLIC LAUNCHER_VERSION=L"${PROJECT_VERSION}")

# launcher.json and the covers go into the exe as well (src/core/embedded.h), so
# the launcher works as a single file; files next to the exe still win.
file(GLOB EMBEDDED_COVERS CONFIGURE_DEPENDS "${REPO_ROOT}/launcher/covers/*")
set(EMBEDDED_FILES "${REPO_ROOT}/launcher/launcher.json" ${EMBEDDED_COVERS})

if(NOT WIN32)
  # Linux twins of the Windows-only parts (src/core/posix/), with the same functions.
  target_sources(launcher_core PRIVATE src/core/posix/posix.cpp src/core/posix/unzip.cpp src/core/posix/http.cpp)
  # <winsock2.h> and the like, as one-line stand-ins for win32.h.
  target_include_directories(launcher_core PUBLIC src/core/posix/include)
  # _wtoi and the other Microsoft C runtime names, in every C++ file.
  target_compile_options(launcher_core PUBLIC "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/src/core/posix/crt.h>")
  # The embedded files, by the same names the Windows resources have ("F_" + path, upper-cased).
  set(EMBED_S "${CMAKE_BINARY_DIR}/embedded_files.S")
  file(WRITE "${EMBED_S}" ".section .rodata
")
  set(EMBED_TABLE "")
  foreach(f ${EMBEDDED_FILES})
    file(RELATIVE_PATH rel "${REPO_ROOT}/launcher" "${f}")
    string(TOUPPER "${rel}" name)
    string(REGEX REPLACE "[^A-Z0-9]" "_" name "${name}")
    file(APPEND "${EMBED_S}" ".balign 16
fl_res_${name}: .incbin \"${f}\"
fl_res_${name}_end:
")
    string(APPEND EMBED_TABLE ".quad fl_name_${name}, fl_res_${name}, fl_res_${name}_end
")
    file(APPEND "${EMBED_S}" "fl_name_${name}: .asciz \"F_${name}\"
")
  endforeach()
  file(APPEND "${EMBED_S}" ".balign 8
.globl fl_embedded_table
fl_embedded_table:
${EMBED_TABLE}.quad 0, 0, 0
.section .note.GNU-stack,\"\",@progbits
")
  set_source_files_properties("${EMBED_S}" PROPERTIES OBJECT_DEPENDS "${EMBEDDED_FILES}")
  target_sources(launcher_core PRIVATE "${EMBED_S}")
  find_package(Threads REQUIRED)
  find_package(ZLIB REQUIRED)  # zip packages (src/core/posix/unzip.cpp)
  target_link_libraries(launcher_core PUBLIC Threads::Threads ZLIB::ZLIB)

  add_executable(launcher src/app/main.cpp src/app/cli.cpp src/app/cli_json.cpp src/app/lobby_cli.cpp src/ui/launcher_headless.cpp)
  set_target_properties(launcher PROPERTIES OUTPUT_NAME "${LAUNCHER_NAME}")
  target_link_libraries(launcher PRIVATE launcher_core)
  enable_testing()
  add_executable(launcher_tests tests/test_main.cpp tests/test_core.cpp tests/test_mod.cpp tests/test_shipped_mods.cpp tests/test_site.cpp tests/test_guide.cpp tests/test_relay.cpp tests/test_lobby.cpp)
  target_link_libraries(launcher_tests PRIVATE launcher_core)
  add_test(NAME core_and_relay COMMAND launcher_tests)
  add_custom_command(TARGET launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${REPO_ROOT}/launcher/launcher.json" "$<TARGET_FILE_DIR:launcher>/launcher.json"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${REPO_ROOT}/launcher/covers" "$<TARGET_FILE_DIR:launcher>/covers")
  return()
endif()

target_link_libraries(launcher_core PUBLIC bcrypt shell32 ole32 uuid advapi32 user32 ws2_32 iphlpapi winhttp)

# ---- launcher executable --------------------------------------------------
set(APP_RC "${CMAKE_BINARY_DIR}/app.rc")
if(EXISTS "${LAUNCHER_ICON}")
  file(TO_CMAKE_PATH "${LAUNCHER_ICON}" ICON_PATH)
  file(WRITE "${APP_RC}" "1 ICON \"${ICON_PATH}\"\n")
else()
  file(WRITE "${APP_RC}" "// no icon\n")
endif()
# Version shown in the exe's Properties > Details, and passed to the code.
set(V "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
file(APPEND "${APP_RC}" "1 VERSIONINFO\nFILEVERSION ${V}\nPRODUCTVERSION ${V}\nBEGIN\n BLOCK \"StringFileInfo\"\n BEGIN\n  BLOCK \"040904B0\"\n  BEGIN\n   VALUE \"CompanyName\", \"Multiplayer Everything\"\n   VALUE \"FileDescription\", \"Multiplayer Everything Launcher\"\n   VALUE \"FileVersion\", \"${PROJECT_VERSION}\"\n   VALUE \"ProductName\", \"Multiplayer Everything Launcher\"\n   VALUE \"ProductVersion\", \"${PROJECT_VERSION}\"\n   VALUE \"OriginalFilename\", \"${LAUNCHER_NAME}.exe\"\n  END\n END\n BLOCK \"VarFileInfo\"\n BEGIN\n  VALUE \"Translation\", 0x409, 1200\n END\nEND\n")

foreach(f ${EMBEDDED_FILES})
  file(RELATIVE_PATH rel "${REPO_ROOT}/launcher" "${f}")
  string(TOUPPER "${rel}" name)
  string(REGEX REPLACE "[^A-Z0-9]" "_" name "${name}")
  file(APPEND "${APP_RC}" "F_${name} RCDATA \"${f}\"\n")
endforeach()
set_source_files_properties("${APP_RC}" PROPERTIES OBJECT_DEPENDS "${EMBEDDED_FILES}")

add_executable(launcher WIN32
  src/app/main.cpp
  src/app/cli.cpp
  src/app/cli_json.cpp
  src/app/lobby_cli.cpp
  src/ui/dialogs.cpp
  src/ui/theme.cpp
  src/ui/launcher.cpp
  src/ui/launcher_actions.cpp
  src/ui/launcher_events.cpp
  src/ui/launcher_headless.cpp
  src/ui/launcher_layout.cpp
  src/ui/launcher_lobby.cpp
  src/ui/launcher_mods.cpp
  src/ui/launcher_motion.cpp
  src/ui/launcher_online.cpp
  src/ui/launcher_paint.cpp
  src/ui/launcher_site.cpp
  src/ui/launcher_guide.cpp
  src/ui/settings.cpp
  src/app/app.manifest
  "${APP_RC}"
)
set_target_properties(launcher PROPERTIES OUTPUT_NAME "${LAUNCHER_NAME}")
target_link_libraries(launcher PRIVATE launcher_core gdiplus comctl32 comdlg32 uxtheme dwmapi gdi32 windowscodecs)

# ---- tests ----------------------------------------------------------------
enable_testing()
add_executable(launcher_tests tests/test_main.cpp tests/test_core.cpp tests/test_mod.cpp tests/test_shipped_mods.cpp tests/test_site.cpp tests/test_guide.cpp tests/test_relay.cpp tests/test_lobby.cpp)
target_link_libraries(launcher_tests PRIVATE launcher_core shell32)
add_test(NAME core_and_relay COMMAND launcher_tests)
add_test(NAME end_to_end
  COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/tests/e2e.ps1"
          -Launcher "$<TARGET_FILE:launcher>" -Root "${REPO_ROOT}")
set_tests_properties(end_to_end PROPERTIES LABELS "ui" RUN_SERIAL TRUE)

# Run straight from the build folder: the launcher reads launcher.json next to the exe.
add_custom_command(TARGET launcher POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${REPO_ROOT}/launcher/launcher.json" "$<TARGET_FILE_DIR:launcher>/launcher.json"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${REPO_ROOT}/launcher/covers" "$<TARGET_FILE_DIR:launcher>/covers")
