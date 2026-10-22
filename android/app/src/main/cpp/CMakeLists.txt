cmake_minimum_required(VERSION 3.22.1)
project(pixi_native LANGUAGES C CXX)

include(FetchContent)
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../" ABSOLUTE)

# JS 引擎二选一(宿主只 include src/android/jsc_shim.h,两引擎共用同一张 JSC C API 面):
#   v8      预编译 libv8_monolith.a(cocos-engine-external,tools/fetch_v8_prebuilt.sh 拉取到 third_party/v8-prebuilt,git 忽略)
#   quickjs 源码 FetchContent 编译(2026-09 之前的唯一引擎,留作对照/回退)
set(PIXINATIVE_JS_ENGINE "v8" CACHE STRING "Android JS engine: v8 | quickjs")
if(NOT PIXINATIVE_JS_ENGINE STREQUAL "v8" AND NOT PIXINATIVE_JS_ENGINE STREQUAL "quickjs")
    message(FATAL_ERROR "PIXINATIVE_JS_ENGINE 只能是 v8 或 quickjs(当前 ${PIXINATIVE_JS_ENGINE})")
endif()
message(STATUS "[js-engine] ${PIXINATIVE_JS_ENGINE} (${ANDROID_ABI})")

if(PIXINATIVE_JS_ENGINE STREQUAL "v8")
    set(V8_PREBUILT_ROOT "${PROJECT_ROOT}/third_party/v8-prebuilt")
    set(V8_LIB "${V8_PREBUILT_ROOT}/android/${ANDROID_ABI}/v8/libv8_monolith.a")
    # Existence alone can reuse the old 4 KB V8 static library: ELF alignment
    # cannot detect its internal flags-memory page-size assumptions. Verify the
    # pinned upstream 16 KB fix and checksums on every configuration.
    execute_process(COMMAND bash "${PROJECT_ROOT}/tools/fetch_v8_prebuilt.sh" "${V8_PREBUILT_ROOT}" RESULT_VARIABLE v8_fetch_rc)
    if(NOT v8_fetch_rc EQUAL 0 OR NOT EXISTS "${V8_LIB}")
        message(FATAL_ERROR "预编译 V8 不可用:${V8_LIB}\n手动执行 tools/fetch_v8_prebuilt.sh")
    endif()
    add_library(v8_monolith STATIC IMPORTED)
    set_target_properties(v8_monolith PROPERTIES
        IMPORTED_LOCATION "${V8_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${V8_PREBUILT_ROOT}/android/${ANDROID_ABI}/include/v8")
    # 与 cocos-engine-external/android/CMakeLists.txt 一致:64 位 ABI 的库带指针压缩,头文件里的对象布局必须同步定义
    if(ANDROID_ABI STREQUAL "arm64-v8a" OR ANDROID_ABI STREQUAL "x86_64")
        set_property(TARGET v8_monolith APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS V8_COMPRESS_POINTERS)
    endif()
else()
    # Commit-pinned dependencies need history; a shallow clone may omit the pinned commit.
    FetchContent_Declare(quickjs
        GIT_REPOSITORY https://github.com/bellard/quickjs.git
        GIT_TAG 04be246001599f5995fa2f2d8c91a0f198d3f34c)
    FetchContent_GetProperties(quickjs)
    if(NOT quickjs_POPULATED)
        FetchContent_Populate(quickjs)
    endif()
endif()

set(stb_SOURCE_DIR "${PROJECT_ROOT}/third_party/stb")

if(PIXINATIVE_JS_ENGINE STREQUAL "quickjs")
    add_library(quickjs STATIC
        ${quickjs_SOURCE_DIR}/quickjs.c
        ${quickjs_SOURCE_DIR}/dtoa.c
        ${quickjs_SOURCE_DIR}/libregexp.c
        ${quickjs_SOURCE_DIR}/libunicode.c
        ${quickjs_SOURCE_DIR}/cutils.c)
    target_include_directories(quickjs PRIVATE ${quickjs_SOURCE_DIR})
    target_compile_definitions(quickjs PRIVATE _GNU_SOURCE CONFIG_VERSION="2026-06-04")
    target_compile_options(quickjs PRIVATE "$<$<CONFIG:Debug>:-O2>")
    set_target_properties(quickjs PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()


# 官方 spine-cpp 4.2 运行时(spinex 全量 native 化依赖;与 mac/iOS 同一份 third_party/spine-cpp)
file(GLOB SPINE_CPP_SOURCES CONFIGURE_DEPENDS "${PROJECT_ROOT}/third_party/spine-cpp/spine/*.cpp")
add_library(spine_cpp STATIC ${SPINE_CPP_SOURCES})
target_include_directories(spine_cpp PUBLIC "${PROJECT_ROOT}/third_party/spine-cpp")
target_compile_options(spine_cpp PRIVATE "$<$<CONFIG:Debug>:-O2>")
set_target_properties(spine_cpp PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(pixi_native SHARED native_runtime.cpp
    "${PROJECT_ROOT}/src/host/host_core_common.cpp"   # 三端共用宿主核心(与 mac/iOS 同一份源文件)
    "${PROJECT_ROOT}/src/third_party/zstddeclib.c")
target_include_directories(pixi_native PRIVATE "${PROJECT_ROOT}/src" "${stb_SOURCE_DIR}")
target_link_options(pixi_native PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")
if(PIXINATIVE_JS_ENGINE STREQUAL "v8")
    target_compile_definitions(pixi_native PRIVATE PIXI_JS_ENGINE_V8=1)
    target_link_libraries(pixi_native PRIVATE v8_monolith)
else()
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/quickjs_include")
    configure_file("${quickjs_SOURCE_DIR}/quickjs.h" "${CMAKE_CURRENT_BINARY_DIR}/quickjs_include/quickjs.h" COPYONLY)
    target_include_directories(pixi_native PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/quickjs_include")
    target_compile_definitions(pixi_native PRIVATE PIXI_ANDROID_QUICKJS=1)
    target_link_libraries(pixi_native PRIVATE quickjs)
endif()
target_compile_options(pixi_native PRIVATE "$<$<CONFIG:Debug>:-O2>")
option(PIXINATIVE_PRODUCTION "Production build: kProductionBuild=true(≙ ios/CMakeLists.txt Release 配置)" OFF)
option(PIXINATIVE_DISABLE_ASTC "Force the PNG/WebP path for non-ASTC device testing" OFF)
if(PIXINATIVE_DISABLE_ASTC)
    target_compile_definitions(pixi_native PRIVATE PIXINATIVE_DISABLE_ASTC=1)
endif()
if(PIXINATIVE_PRODUCTION)
    target_compile_definitions(pixi_native PRIVATE PIXINATIVE_PRODUCTION=1)
endif()
# jnigraphics:AndroidBitmap_lockPixels(文本位图零拷贝上传);z:zip/fgui 包 raw deflate + crc32
target_link_libraries(pixi_native PRIVATE spine_cpp android jnigraphics log EGL GLESv3 z m)
