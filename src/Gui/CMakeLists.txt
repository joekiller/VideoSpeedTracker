# VST Studio: the desktop app around the tracker (Dear ImGui on GLFW + OpenGL 3).
#
# Everything it needs beyond OpenCV is fetched at configure time at pinned versions and linked statically, so the
# release packages gain one executable and no new runtime dependencies:
#   GLFW 3.4 (zlib), Dear ImGui 1.91.9b (MIT), portable-file-dialogs (WTFPL; native dialogs on Windows and macOS,
#   zenity or kdialog on Linux), Inter 4.1 and Font Awesome Free 6.7.2 solid (both SIL OFL 1.1, embedded).
include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
# X11 only on Linux: it runs under XWayland on Wayland desktops and saves the wayland-scanner build dependency.
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
  URL https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip
  URL_HASH SHA256=b5ec004b2712fd08e8861dc271428f048775200a2df719ccf575143ba749a3e9)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.9b
  GIT_SHALLOW TRUE)
# Header-only; SOURCE_SUBDIR points at nothing so its examples aren't added to the build.
FetchContent_Declare(pfd
  GIT_REPOSITORY https://github.com/samhocevar/portable-file-dialogs.git
  GIT_TAG c12ea8c9a727f5320a2b4570aee863bbede2a204
  SOURCE_SUBDIR none)
FetchContent_Declare(inter
  URL https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip
  URL_HASH SHA256=9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e
  SOURCE_SUBDIR none)
FetchContent_MakeAvailable(glfw imgui pfd inter)

# file(DOWNLOAD) with a few retries: CI runners occasionally fail to connect to raw.githubusercontent.com, and
# a single dropped connection shouldn't fail the whole configure.
function(vst_download url dest hash)
  foreach(attempt 1 2 3 4)
    file(DOWNLOAD ${url} ${dest}.part STATUS st TLS_VERIFY ON)
    list(GET st 0 code)
    if(code EQUAL 0)
      file(SHA256 ${dest}.part got)
      if(got STREQUAL hash)
        file(RENAME ${dest}.part ${dest})
        return()
      endif()
      message(FATAL_ERROR "${url}: SHA256 ${got}, expected ${hash}")
    endif()
    message(STATUS "Download of ${url} failed (${st}), attempt ${attempt}")
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep ${attempt})
  endforeach()
  message(FATAL_ERROR "Could not download ${url}: ${st}")
endfunction()

set(FA_DIR ${CMAKE_BINARY_DIR}/_deps/fontawesome)
if(NOT EXISTS ${FA_DIR}/fa-solid-900.ttf)
  vst_download(https://raw.githubusercontent.com/FortAwesome/Font-Awesome/6.7.2/webfonts/fa-solid-900.ttf
    ${FA_DIR}/fa-solid-900.ttf af19d135d3a935b3ebfbd80320716ffe1202052c5f68dc2c5f1abc57005ac605)
  vst_download(https://raw.githubusercontent.com/FortAwesome/Font-Awesome/6.7.2/LICENSE.txt
    ${FA_DIR}/LICENSE.txt 9b914eae88817d63b576eab5aafde7068c7a1abae125d7cdfb034f1da43a9afc)
endif()

# Single-file libraries for the companion API: cpp-httplib 0.18.3 (MIT), nlohmann/json 3.11.3 (MIT) and
# Nayuki's QR Code generator 1.8.0 (MIT) for the pairing code.
set(SINGLE_DIR ${CMAKE_BINARY_DIR}/_deps/single)
foreach(entry IN ITEMS
    "https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.18.3/httplib.h|httplib.h|a0a0c13dc086663863dbe6730a19716f7d3744904b6205496e8301750814dbd5"
    "https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.18.3/LICENSE|LICENSE.httplib|4b45cbe16d7b71b89ae6127e26e0d90a029198ca5e958ad8e3d0b8bbed364d8b"
    "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp|nlohmann/json.hpp|9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6"
    "https://raw.githubusercontent.com/nlohmann/json/v3.11.3/LICENSE.MIT|LICENSE.json|86b998c792894ccb911a1cb7994f7a9652894e7a094c0b5e45be2f553f45cf14"
    "https://raw.githubusercontent.com/nayuki/QR-Code-generator/v1.8.0/cpp/qrcodegen.cpp|qrcodegen.cpp|1f3b3fcdac6954c32cf583ccd02ec9b5901f756a38c461acedc70be4a77d3757"
    "https://raw.githubusercontent.com/nayuki/QR-Code-generator/v1.8.0/cpp/qrcodegen.hpp|qrcodegen.hpp|b779c3b156cf7a57ce789d6fee4fc991ccc2913774d26c909d22bb8f26b2a793")
  string(REPLACE "|" ";" parts "${entry}")
  list(GET parts 0 url)
  list(GET parts 1 name)
  list(GET parts 2 hash)
  if(NOT EXISTS ${SINGLE_DIR}/${name})
    vst_download(${url} ${SINGLE_DIR}/${name} ${hash})
  endif()
endforeach()

# Fonts are compiled in, so the app never has to find data files next to itself.
set(EMBEDDED)
foreach(font IN ITEMS
    "${inter_SOURCE_DIR}/extras/ttf/Inter-Regular.ttf|font_inter_regular"
    "${inter_SOURCE_DIR}/extras/ttf/Inter-SemiBold.ttf|font_inter_semibold"
    "${inter_SOURCE_DIR}/extras/ttf/Inter-Bold.ttf|font_inter_bold"
    "${FA_DIR}/fa-solid-900.ttf|font_fa_solid")
  string(REPLACE "|" ";" parts "${font}")
  list(GET parts 0 in)
  list(GET parts 1 name)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/${name}.cpp)
  add_custom_command(OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -DIN=${in} -DOUT=${out} -DNAME=${name} -P ${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake
    DEPENDS ${in} ${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake
    COMMENT "Embedding ${name}")
  list(APPEND EMBEDDED ${out})
endforeach()

add_library(imgui_lib STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui_lib PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui_lib PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DEFINE_MATH_OPERATORS)
target_link_libraries(imgui_lib PUBLIC glfw)

# The tracker compiled for embedding: same sources and flags as the CLI, with main() replaced by vst::run().
add_library(vst_engine STATIC
  ${VST_DIR}/videoSpeedTracker.cpp
  ${VST_DIR}/Globals.cpp
  ${VST_DIR}/VehicleDynamics.cpp
  ${VST_DIR}/Projection.cpp
  ${VST_DIR}/Snapshot.cpp
  ${VST_DIR}/PiCamera.cpp
  ${VST_DIR}/FrameInput.cpp)
target_include_directories(vst_engine PUBLIC ${VST_DIR})
target_compile_definitions(vst_engine PUBLIC VST_EMBEDDED PRIVATE VST_VERSION="${VST_VERSION}")
target_link_libraries(vst_engine PUBLIC vst_common ${OpenCV_LIBS})
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(vst_engine PRIVATE -ffp-contract=off)
endif()

# The street report (Report.cpp), the stats csv reader it builds on and the unattended-run health checks
# (Health.cpp), the site planner's model (Plan.cpp) and combining calibration inputs (CalEvidence.cpp): no ImGui or OpenGL, so tests/report_test.cpp,
# tests/health_test.cpp and tests/plan_test.cpp link it on their own.
add_library(vst_report STATIC Report.cpp Results.cpp Health.cpp DashCam.cpp Plan.cpp CalEvidence.cpp Resume.cpp Archive.cpp
  CalSets.cpp Dataset.cpp Sha256.cpp)
target_include_directories(vst_report PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${SINGLE_DIR})
target_link_libraries(vst_report PUBLIC ${OpenCV_LIBS})
if(MSVC)
  target_compile_options(vst_report PRIVATE /utf-8)
endif()

find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
add_executable(VSTStudio WIN32
  main.cpp
  App.cpp
  AppHealth.cpp
  Canvas.cpp
  Media.cpp
  Guide.cpp
  Calibration.cpp
  Sets.cpp
  Server.cpp
  Settings.cpp
  Theme.cpp
  Tracker.cpp
  ${SINGLE_DIR}/qrcodegen.cpp
  ${EMBEDDED})
target_include_directories(VSTStudio PRIVATE ${pfd_SOURCE_DIR} ${SINGLE_DIR})
if(WIN32)
  # The app icon (packaging/windows/vst-studio.ico, made from packaging/vst-studio.png); rc.exe takes forward slashes.
  set(VST_STUDIO_ICO ${CMAKE_SOURCE_DIR}/packaging/windows/vst-studio.ico)
  configure_file(VSTStudio.rc.in ${CMAKE_CURRENT_BINARY_DIR}/VSTStudio.rc @ONLY)
  target_sources(VSTStudio PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/VSTStudio.rc)
endif()
# Long-lived MJPEG and server-sent-event connections each hold a worker, so allow more than the default 8.
target_compile_definitions(VSTStudio PRIVATE CPPHTTPLIB_THREAD_POOL_COUNT=16)
target_compile_definitions(VSTStudio PRIVATE VST_VERSION="${VST_VERSION}")
target_link_libraries(VSTStudio PRIVATE vst_engine vst_report imgui_lib OpenGL::GL Threads::Threads)
if(WIN32)
  # Keep windows.h from defining min/max macros and from dragging in winsock 1 ahead of cpp-httplib's winsock2.
  target_compile_definitions(VSTStudio PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _HAS_STD_BYTE=0)
endif()
if(MSVC)
  # Sources hold UTF-8 text (arrows, ×, ±); WIN32 above gives a GUI-subsystem exe with a standard main() entry.
  target_compile_options(VSTStudio PRIVATE /utf-8)
  target_link_options(VSTStudio PRIVATE /ENTRY:mainCRTStartup)
endif()
if(APPLE)
  target_compile_definitions(VSTStudio PRIVATE GL_SILENCE_DEPRECATION)
  target_compile_definitions(imgui_lib PRIVATE GL_SILENCE_DEPRECATION)
endif()

install(TARGETS VSTStudio RUNTIME DESTINATION ${VST_BIN_DIR})
if(WIN32 OR APPLE)
  set(VST_GUI_NOTICE_DIR licenses)
else()
  set(VST_GUI_NOTICE_DIR ${CMAKE_INSTALL_DATADIR}/doc/videospeedtracker)
  install(FILES ${CMAKE_SOURCE_DIR}/packaging/vst-studio.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES ${CMAKE_SOURCE_DIR}/packaging/vst-studio.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)
endif()
install(FILES ${glfw_SOURCE_DIR}/LICENSE.md DESTINATION ${VST_GUI_NOTICE_DIR}/glfw)
install(FILES ${imgui_SOURCE_DIR}/LICENSE.txt DESTINATION ${VST_GUI_NOTICE_DIR}/imgui)
install(FILES ${pfd_SOURCE_DIR}/COPYING DESTINATION ${VST_GUI_NOTICE_DIR}/portable-file-dialogs)
install(FILES ${inter_SOURCE_DIR}/LICENSE.txt DESTINATION ${VST_GUI_NOTICE_DIR}/inter-font)
install(FILES ${FA_DIR}/LICENSE.txt DESTINATION ${VST_GUI_NOTICE_DIR}/font-awesome)
install(FILES ${SINGLE_DIR}/LICENSE.httplib DESTINATION ${VST_GUI_NOTICE_DIR}/cpp-httplib RENAME LICENSE)
install(FILES ${SINGLE_DIR}/LICENSE.json DESTINATION ${VST_GUI_NOTICE_DIR}/nlohmann-json RENAME LICENSE)
install(FILES ${CMAKE_SOURCE_DIR}/packaging/qrcodegen-LICENSE.txt DESTINATION ${VST_GUI_NOTICE_DIR}/qrcodegen RENAME LICENSE)
