cmake_minimum_required(VERSION 3.16)

# The Vita toolchain must be picked up before project() so that the cross
# compiler, sysroot and stub libraries are in place. This mirrors the layout of
# the official vitasdk samples.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
    else()
        message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
    endif()
endif()

project(VitaMusic C)

include("${VITASDK}/share/vita.cmake" REQUIRED)

# --- LiveArea / bubble metadata -------------------------------------------
set(VITA_APP_NAME "VitaMusic")
# Must be exactly 9 characters: developer prefix + app number.
set(VITA_TITLEID "VTAMUSIC1")
set(VITA_VERSION "01.00")
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1")

# --- Compiler flags --------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter -O2 -g")

# --- Sources ---------------------------------------------------------------
add_executable(${PROJECT_NAME}
    src/main.c
    src/app.c
    src/ui.c
    src/util.c
    src/net.c
    src/innertube.c
    src/lyrics.c
    src/json.c
    src/mp4.c
    src/decoder.c
    src/audio.c
    src/player.c
)

# The Vita sysroot already carries arm-vita-eabi/lib on the link path, so
# libraries provided by vdpm packages are linked by plain name.
#
# Note that mpg123 is deliberately absent: YouTube Music only serves AAC and
# Opus, so decoding goes through the hardware codec (SceAudiodec) instead.
#
# libcurl in the SDK is built against OpenSSL and enabled for HTTP compression,
# so ssl/crypto/z/zstd have to follow it, and because these are static archives
# they are wrapped in a group so the linker can resolve them in any order.
target_link_libraries(${PROJECT_NAME}
    vita2d
    m
    SceAppMgr_stub
    SceAppUtil_stub
    SceAudio_stub
    SceAudiodec_stub
    SceCommonDialog_stub
    SceCtrl_stub
    SceDisplay_stub
    SceGxm_stub
    SceIme_stub
    SceIofilemgr_stub
    SceKernelThreadMgr_stub
    SceKernelModulemgr_stub
    SceLibKernel_stub
    SceNet_stub
    SceNetCtl_stub
    ScePgf_stub
    ScePvf_stub
    SceProcessmgr_stub
    ScePower_stub
    SceRtc_stub
    SceSysmem_stub
    SceSysmodule_stub
    SceTouch_stub
)

# libvita2d carries its own PNG/JPEG/font backends, so those archives go in the
# same group.
target_link_libraries(${PROJECT_NAME}
    -Wl,--start-group
    curl
    ssl
    crypto
    png16
    jpeg
    freetype
    z
    zstd
    -Wl,--end-group
)

# --- Vita artifacts --------------------------------------------------------
# UNSAFE gives the app the full SDK privileges homebrew expects (networking,
# unrestricted filesystem). It requires HENkaku/Enso on the console.
vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE)

# The LiveArea artwork is optional so that a build without icon0.png still
# produces a usable VPK instead of failing on a missing file.
#
# The Vita installer refuses packages whose sce_sys artwork is not an 8-bit
# indexed (palette) PNG of exactly the right size and fails with error
# 0x8010113D, so validate the PNG header here: bytes 16-23 are the big-endian
# width/height, byte 24 the bit depth and byte 25 the color type (3 = indexed).
# Expected PNG dimensions: hex big-endian IHDR width/height per asset.
set(_exp_icon0.png "0000008000000080")            # 128x128
set(_exp_livearea/contents/bg.png "00000348000001f4")   # 840x500
set(_exp_livearea/contents/startup.png "000001180000009e") # 280x158
set(VITA_PKG_FILES)
foreach(_asset icon0.png livearea/contents/bg.png livearea/contents/startup.png)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/sce_sys/${_asset}")
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/assets/sce_sys/${_asset}" _png_hex HEX)
        string(SUBSTRING "${_png_hex}" 32 16 _png_dims_hex)
        string(SUBSTRING "${_png_hex}" 48 2 _png_bit_depth)
        string(SUBSTRING "${_png_hex}" 50 2 _png_color_type)
        if(NOT _png_color_type STREQUAL "03" OR NOT _png_bit_depth STREQUAL "08")
            message(FATAL_ERROR
                "VitaMusic: assets/sce_sys/${_asset} is not an 8-bit indexed PNG "
                "(bit depth 0x${_png_bit_depth}, color type 0x${_png_color_type}). "
                "The Vita installer rejects such packages with error 0x8010113D. "
                "Convert it with 'pngquant 256 <file>' or Pillow: "
                "Image.open(f).convert('RGBA').quantize(256).save(f).")
        endif()
        if(NOT _png_dims_hex STREQUAL _exp_${_asset})
            message(FATAL_ERROR
                "VitaMusic: assets/sce_sys/${_asset} has wrong dimensions "
                "(IHDR ${_png_dims_hex}); the Vita installer requires "
                "${_exp_${_asset}} (see comments above) and rejects other "
                "sizes with error 0x8010113D. Resize it keeping the 8-bit "
                "indexed palette format.")
        endif()
        list(APPEND VITA_PKG_FILES FILE assets/sce_sys/${_asset} sce_sys/${_asset})
    else()
        message(STATUS "VitaMusic: assets/sce_sys/${_asset} missing, skipped")
    endif()
endforeach()
list(APPEND VITA_PKG_FILES
    FILE assets/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    ${VITA_PKG_FILES}
)
