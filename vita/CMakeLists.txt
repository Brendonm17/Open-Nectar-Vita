# Open Nectar, PS Vita build (VitaSDK + vitaGL).
#
#   cmake -S vita -B vita/build -G Ninja
#   cmake --build vita/build
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "VitaSDK toolchain")
    else()
        message(FATAL_ERROR "VITASDK not set")
    endif()
endif()

project(nectar_vita LANGUAGES C CXX)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
endif()

set(VITA_APP_NAME "Pikmin")
set(VITA_TITLEID  "NECTAR001")
set(VITA_VERSION  "01.00")

get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(PC_PORT_DIR "${ROOT}/pc_port")

option(PIKMIN_NATIVE_JAUDIO "Original JAudio engine" ON)
option(NECTAR_LOGGING "Runtime log (ux0:data/Pikmin/nectar.log)" ON)
option(NECTAR_LOG_CONSOLE "Mirror the log to the debug console" ON)
option(NECTAR_DEBUG_UNLOCK "Debug: unlock all stages" OFF)
option(NECTAR_TESTER "Log only missing-shader captures" OFF)
option(NECTAR_GXQ_LEAN "Lean GX record core" ON)
option(NECTAR_GXQ_SLOW_COLD "Mark GX record slow paths cold" OFF)
option(NECTAR_LTO "Link-time optimisation" ON)
option(NECTAR_PROF "Function-level sampling profiler" OFF)

if(NOT NECTAR_LOGGING)
    add_compile_definitions(NECTAR_LOGGING=0)
endif()
if(NOT NECTAR_LOG_CONSOLE)
    add_compile_definitions(NECTAR_LOG_CONSOLE=0)
endif()
if(NECTAR_DEBUG_UNLOCK)
    add_compile_definitions(NECTAR_DEBUG_UNLOCK=1)
    message(WARNING "NECTAR_DEBUG_UNLOCK is ON: debug build, do not ship")
endif()
if(NECTAR_TESTER)
    add_compile_definitions(NECTAR_TESTER_BUILD=1)
endif()
add_compile_definitions(
    NECTAR_GXQ_LEAN=$<BOOL:${NECTAR_GXQ_LEAN}>
    NECTAR_GXQ_SLOW_COLD=$<BOOL:${NECTAR_GXQ_SLOW_COLD}>
)

add_compile_definitions(
    PIKI_PC_PORT=1
    VERSION_GPIE01_01
    DTK_CONFIG_NONMATCHING=1
    PIKI_USE_DGX=1
    PIKI_USE_JAUDIO=$<BOOL:${PIKMIN_NATIVE_JAUDIO}>
    SDL_MAIN_HANDLED=1
    PIKI_PC_SETTINGS_MENU=1
    NECTAR_VITA=1
    NECTAR_IMAGE_ZSTD=1
    NECTAR_IMAGE_LZMA=1
    NECTAR_IMAGE_BZIP2=1
)

# vita/include first: vitaGL GL headers.
include_directories(
    ${CMAKE_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${ROOT}/include
    ${ROOT}/include/Dolphin
    ${ROOT}/include/Dolphin/OS
    ${ROOT}/include/Dolphin/GX
    ${PC_PORT_DIR}
    ${PC_PORT_DIR}/gl
    $ENV{VITASDK}/arm-vita-eabi/include/SDL2
)

# -fno-short-enums: GameCube enum size.
set(COMMON_FLAGS "-fno-short-enums -fsigned-char -fno-strict-aliasing -fwrapv -ffunction-sections -fdata-sections -fno-math-errno")
set(NECTAR_LTO_FLAGS "")
set(NECTAR_LTO_LINK_FLAGS "")
if(NECTAR_LTO)
    set(NECTAR_LTO_FLAGS "-flto -ffat-lto-objects")
    set(NECTAR_LTO_LINK_FLAGS "-flto=auto -ffat-lto-objects")
endif()
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -O2 -g1 ${COMMON_FLAGS} ${NECTAR_LTO_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -g1 ${COMMON_FLAGS} -fpermissive ${NECTAR_LTO_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${NECTAR_LTO_LINK_FLAGS}")

set(LEGACY_COMPILE_OPTIONS
    "SHELL:-include ${PC_PORT_DIR}/pc_types.h"
    -fpermissive -w
)
set(NATIVE_COMPILE_OPTIONS
    "SHELL:-include ${PC_PORT_DIR}/pc_types.h"
    -fpermissive
    -Wno-narrowing -Wno-multichar -Wno-write-strings
    -Werror=return-type
)

# Baked shaders.
set(GXP_HEADER ${CMAKE_BINARY_DIR}/vita_gxp_shaders.h)
set(GXP_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/shaders/manifest.json)
file(GLOB GXP_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gxp/*.gxp")
add_custom_command(
    OUTPUT ${GXP_HEADER}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_gxp_header.py ${GXP_MANIFEST} ${GXP_HEADER}
    WORKING_DIRECTORY ${ROOT}
    DEPENDS ${GXP_MANIFEST} ${GXP_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_gxp_header.py
    COMMENT "Generating vita_gxp_shaders.h"
)

file(GLOB SYSCOMMON_SOURCES "${ROOT}/src/sysCommon/*.cpp")
set(SYSDOLPHIN_SOURCES
    ${ROOT}/src/sysDolphin/controllerMgr.cpp
    ${ROOT}/src/sysDolphin/dgxGraphics.cpp
    ${ROOT}/src/sysDolphin/gameApp.cpp
    ${ROOT}/src/sysDolphin/sysNew.cpp
    ${ROOT}/src/sysDolphin/system.cpp
    ${ROOT}/src/sysDolphin/texture.cpp
)
file(GLOB PLUGPIKI_SOURCES
    "${ROOT}/src/plugPikiKando/*.cpp"
    "${ROOT}/src/plugPikiNakata/*.cpp"
    "${ROOT}/src/plugPikiNishimura/*.cpp"
    "${ROOT}/src/plugPikiColin/*.cpp"
    "${ROOT}/src/plugPikiOgawa/*.cpp"
    "${ROOT}/src/plugPikiYamashita/*.cpp"
)

set(PC_PORT_SOURCES
    ${PC_PORT_DIR}/pc_main.cpp
    ${PC_PORT_DIR}/pc_window.cpp
    ${PC_PORT_DIR}/pc_pad_axis.cpp
    ${PC_PORT_DIR}/pc_menu_repeat.cpp
    ${PC_PORT_DIR}/pc_photo_mode.cpp
    ${PC_PORT_DIR}/gl/pc_gfx.cpp
    ${PC_PORT_DIR}/gl/vita_gxq.cpp
    ${PC_PORT_DIR}/gl/pc_tev_shader.cpp
    ${PC_PORT_DIR}/timing/pc_frame_scheduler.cpp
    ${PC_PORT_DIR}/timing/pc_render_phase.cpp
    ${PC_PORT_DIR}/timing/pc_visual_snapshot.cpp
    ${PC_PORT_DIR}/timing/pc_camera_snapshot.cpp
    ${PC_PORT_DIR}/timing/pc_visual_runtime.cpp
    ${PC_PORT_DIR}/timing/pc_render_packet.cpp
    ${PC_PORT_DIR}/timing/pc_prof.cpp
    ${PC_PORT_DIR}/audio/pc_audio.cpp
    ${PC_PORT_DIR}/audio/pc_envelope.cpp
    ${PC_PORT_DIR}/audio/pc_jam.cpp
    ${PC_PORT_DIR}/audio/pc_instrument_bank.cpp
    ${PC_PORT_DIR}/audio/pc_wave_bank.cpp
    ${PC_PORT_DIR}/audio/pc_sequence_archive.cpp
    ${PC_PORT_DIR}/audio/pc_aram.cpp
    ${PC_PORT_DIR}/audio/pc_dsp_host.cpp
    ${PC_PORT_DIR}/settings/pc_settings.cpp
    ${ROOT}/src/jaudio/pikiseq.c
    ${ROOT}/src/hvqm4dec/hvqm4dec.c
    ${PC_PORT_DIR}/dolphin_stubs/os_stubs.cpp
    ${PC_PORT_DIR}/dolphin_stubs/gx_stubs.cpp
    ${PC_PORT_DIR}/dolphin_stubs/dvd_stubs.cpp
    ${PC_PORT_DIR}/dolphin_stubs/dvd_image.cpp
    ${PC_PORT_DIR}/dolphin_stubs/pad_stubs.cpp
    ${PC_PORT_DIR}/dolphin_stubs/vi_stubs.cpp
    ${PC_PORT_DIR}/dolphin_stubs/card_stubs.cpp
    ${PC_PORT_DIR}/dolphin_stubs/audio_stubs.cpp
    ${PC_PORT_DIR}/dolphin_stubs/jaudio_hw_stubs.cpp
    ${PC_PORT_DIR}/dolphin_stubs/misc_stubs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vita_platform.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vita_rt.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vita_rt_gx.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vita_gx_shaders.cpp
    ${GXP_HEADER}
)

set(JAUDIO_TARGET_SOURCES "")
if(PIKMIN_NATIVE_JAUDIO)
    file(GLOB JAUDIO_TARGET_SOURCES "${ROOT}/src/jaudio/*.c")
    # GameCube hardware paths, replaced by pc_port/audio.
    list(FILTER JAUDIO_TARGET_SOURCES EXCLUDE REGEX
        ".*/(aictrl|audiothread|cpubuf|dsp_cardunlock|dspboot|dspbuf|dspproc|dsptask|ipldec|osdsp|osdsp_task)\.c$")
    set_source_files_properties(${JAUDIO_TARGET_SOURCES} PROPERTIES LANGUAGE CXX)
    set_source_files_properties(${ROOT}/src/hvqm4dec/hvqm4dec.c PROPERTIES COMPILE_FLAGS "-O3")
    list(REMOVE_ITEM PC_PORT_SOURCES ${ROOT}/src/jaudio/pikiseq.c
        ${PC_PORT_DIR}/audio/pc_dsp_host.cpp ${PC_PORT_DIR}/dolphin_stubs/jaudio_hw_stubs.cpp)
    list(APPEND PC_PORT_SOURCES ${PC_PORT_DIR}/audio/jaudio_integration_test.cpp)
    list(APPEND JAUDIO_TARGET_SOURCES ${PC_PORT_DIR}/audio/jaudio_host.cpp
        ${PC_PORT_DIR}/audio/jaudio_bank_host.cpp ${PC_PORT_DIR}/audio/jaudio_sink.cpp)
endif()

add_library(pikmin_legacy STATIC
    ${SYSCOMMON_SOURCES} ${SYSDOLPHIN_SOURCES} ${PLUGPIKI_SOURCES} ${JAUDIO_TARGET_SOURCES})
target_compile_options(pikmin_legacy PRIVATE ${LEGACY_COMPILE_OPTIONS})

add_executable(${PROJECT_NAME} ${PC_PORT_SOURCES})
target_compile_options(${PROJECT_NAME} PRIVATE ${NATIVE_COMPILE_OPTIONS})
target_link_options(${PROJECT_NAME} PRIVATE
    -Wl,--gc-sections
    -Wl,--undefined=_newlib_heap_size_user
    -Wl,--undefined=sceUserMainThreadStackSize
    # Console output to the file logger.
    -Wl,--wrap=printf -Wl,--wrap=vprintf
    -Wl,--wrap=fprintf -Wl,--wrap=vfprintf
    -Wl,--wrap=puts -Wl,--wrap=fputs
)

if(NECTAR_PROF)
    add_compile_definitions(NECTAR_PROF_BUILD=1)
    set(NECTAR_INSTRUMENT
        -finstrument-functions
        -finstrument-functions-exclude-file-list=/include/
        -finstrument-functions-exclude-function-list=gxq_same,gxq_valid,gxq_emit,wr_i32,wr_u32,wr_f32,rd_i32,rd_u32,rd_f32,vita_offlock_worker,pcMainOnly,gxq_put,gxq_each,gxq_hit,gxq_st1,gxq_st2,gxq_st3,gxq_st4,gxq_st5,gxq_st12,gxq_r_,gxq_color_word,gxq_rd_,pcLd32,pcKeyEq,pcTevKeyEq,gxq_refuse,pcRestoreMatrix,pcBlockVerdict,gxq_ring_,gxq_seg_room)
    target_compile_options(pikmin_legacy PRIVATE ${NECTAR_INSTRUMENT})
    set_source_files_properties(
        ${PC_PORT_DIR}/gl/pc_gfx.cpp
        ${PC_PORT_DIR}/gl/vita_gxq.cpp
        ${PC_PORT_DIR}/dolphin_stubs/gx_stubs.cpp
        PROPERTIES COMPILE_OPTIONS "${NECTAR_INSTRUMENT}")
endif()

# Log wrappers stay out of LTO.
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/vita_platform.cpp PROPERTIES COMPILE_OPTIONS "-fno-lto")

target_link_libraries(${PROJECT_NAME}
    pikmin_legacy
    zstd lzma bz2
    SDL2
    vitaGL vitashark mathneon SceShaccCgExt taihen_stub SceShaccCg_stub
    SceDisplay_stub SceCtrl_stub SceTouch_stub SceHid_stub SceMotion_stub
    SceAudio_stub SceAudioIn_stub SceSysmodule_stub SceGxm_stub
    SceCommonDialog_stub SceAppMgr_stub ScePower_stub SceIme_stub ScePvf_stub
    SceKernelDmacMgr_stub SceLibKernel_stub
    pthread m stdc++
)

# Extended user memory budget.
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE)

# Optional LiveArea assets (not in the repo).
set(VPK_FILES "")
foreach(f icon0.png livearea/contents/bg0.png livearea/contents/startup.png livearea/contents/template.xml)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/${f})
        list(APPEND VPK_FILES FILE sce_sys/${f} sce_sys/${f})
    endif()
endforeach()

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    ${VPK_FILES}
)
