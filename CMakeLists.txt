cmake_minimum_required(VERSION 3.21)
project(vantalock VERSION 0.1.5 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Qt is used for image decoding (hdr_image) and small utilities (hyprctl query,
# JSON), never for windowing.
find_package(Qt6 REQUIRED COMPONENTS Core Gui)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WaylandClient REQUIRED IMPORTED_TARGET wayland-client)
find_program(WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)

find_package(Vulkan REQUIRED)
find_program(GLSLC NAMES glslc REQUIRED)

pkg_check_modules(Xkbcommon REQUIRED IMPORTED_TARGET xkbcommon)
find_library(PAM_LIB NAMES pam REQUIRED)

# Image libraries for the HDR decode pipeline (reused from vantaviewer).
find_library(UHDR_LIB NAMES uhdr ultrahdr REQUIRED)
pkg_check_modules(Avif REQUIRED IMPORTED_TARGET libavif)
pkg_check_modules(Lcms2 REQUIRED IMPORTED_TARGET lcms2)
pkg_check_modules(Jxl REQUIRED IMPORTED_TARGET libjxl)
pkg_check_modules(Heif REQUIRED IMPORTED_TARGET libheif)

# --- Generate Wayland client protocol glue ---------------------------------
set(WL_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/wl-protocols")
file(MAKE_DIRECTORY "${WL_GEN_DIR}")

function(wl_protocol XML BASENAME OUT_SOURCES)
    set(HDR "${WL_GEN_DIR}/${BASENAME}-client-protocol.h")
    set(SRC "${WL_GEN_DIR}/${BASENAME}.c")
    add_custom_command(OUTPUT "${HDR}"
        COMMAND "${WAYLAND_SCANNER}" client-header "${XML}" "${HDR}"
        DEPENDS "${XML}" VERBATIM)
    add_custom_command(OUTPUT "${SRC}"
        COMMAND "${WAYLAND_SCANNER}" private-code "${XML}" "${SRC}"
        DEPENDS "${XML}" VERBATIM)
    set(${OUT_SOURCES} "${HDR}" "${SRC}" PARENT_SCOPE)
endfunction()

wl_protocol("${WAYLAND_PROTOCOLS_DIR}/staging/color-management/color-management-v1.xml"
    "color-management-v1" CM_SOURCES)
wl_protocol("${WAYLAND_PROTOCOLS_DIR}/staging/ext-session-lock/ext-session-lock-v1.xml"
    "ext-session-lock-v1" LOCK_SOURCES)

# --- Compile shaders to SPIR-V embedded as C arrays ------------------------
set(SPV_DIR "${CMAKE_CURRENT_BINARY_DIR}/spv")
file(MAKE_DIRECTORY "${SPV_DIR}")
function(compile_shader STAGE SRC OUT)
    add_custom_command(OUTPUT "${OUT}"
        COMMAND "${GLSLC}" -O -mfmt=c -fshader-stage=${STAGE} "${SRC}" -o "${OUT}"
        DEPENDS "${SRC}" VERBATIM)
endfunction()
compile_shader(vert "${CMAKE_CURRENT_SOURCE_DIR}/shaders/present.vert" "${SPV_DIR}/present.vert.inc")
compile_shader(frag "${CMAKE_CURRENT_SOURCE_DIR}/shaders/present.frag" "${SPV_DIR}/present.frag.inc")
compile_shader(vert "${CMAKE_CURRENT_SOURCE_DIR}/shaders/overlay.vert" "${SPV_DIR}/overlay.vert.inc")
compile_shader(frag "${CMAKE_CURRENT_SOURCE_DIR}/shaders/overlay.frag" "${SPV_DIR}/overlay.frag.inc")

add_executable(vantalock
    src/main.cpp
    src/session_lock.cpp
    src/renderer.cpp
    src/cm_tag.cpp
    src/hdr_image.cpp
    src/overlay_text.cpp
    src/config.cpp
    src/auth.cpp
    ${CM_SOURCES}
    ${LOCK_SOURCES}
)

# renderer.cpp #includes the generated SPIR-V initialisers.
set_source_files_properties(src/renderer.cpp PROPERTIES
    OBJECT_DEPENDS "${SPV_DIR}/present.vert.inc;${SPV_DIR}/present.frag.inc;${SPV_DIR}/overlay.vert.inc;${SPV_DIR}/overlay.frag.inc")

target_include_directories(vantalock PRIVATE "${WL_GEN_DIR}" "${SPV_DIR}")

target_link_libraries(vantalock PRIVATE
    Qt6::Core
    Qt6::Gui
    Vulkan::Vulkan
    PkgConfig::WaylandClient
    PkgConfig::Avif
    PkgConfig::Lcms2
    PkgConfig::Jxl
    PkgConfig::Heif
    PkgConfig::Xkbcommon
    ${UHDR_LIB}
    ${PAM_LIB}
)

include(GNUInstallDirs)
install(TARGETS vantalock RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES pam/vantalock DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/pam.d)
