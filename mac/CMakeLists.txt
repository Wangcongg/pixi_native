cmake_minimum_required(VERSION 3.20)
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum platform version")
project(PixiNativeApp LANGUAGES C CXX OBJCXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_XCODE_GENERATE_SCHEME ON)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
include("${ROOT}/generated/mac/project.cmake")
file(GLOB SPINE_SOURCES CONFIGURE_DEPENDS "${ROOT}/third_party/spine-cpp/spine/*.cpp")
add_library(spine_cpp STATIC ${SPINE_SOURCES})
target_include_directories(spine_cpp PUBLIC "${ROOT}/third_party/spine-cpp")
set_target_properties(spine_cpp PROPERTIES XCODE_ATTRIBUTE_SKIP_INSTALL YES)
add_executable(PixiNativeApp MACOSX_BUNDLE
 "${ROOT}/src/mac/mac_app.mm" "${ROOT}/src/host/host_core.mm" "${ROOT}/src/host/host_core_common.cpp"
 "${ROOT}/src/common/native_image_decode.cpp" "${ROOT}/src/third_party/zstddeclib.c")
target_include_directories(PixiNativeApp PRIVATE "${ROOT}/src" "${ROOT}/src/mac" "${ROOT}/src/cmdbuf")
set_target_properties(PixiNativeApp PROPERTIES
 MACOSX_BUNDLE_INFO_PLIST "${ROOT}/generated/mac/Info.plist"
 XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${KIT_APP_ID}"
 XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${KIT_DEVELOPMENT_TEAM}"
 XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES)
target_compile_options(PixiNativeApp PRIVATE -fobjc-arc)
file(GLOB_RECURSE GAME_FILES CONFIGURE_DEPENDS LIST_DIRECTORIES false "${ROOT}/game/*")
foreach(FILE IN LISTS GAME_FILES)
 file(RELATIVE_PATH RELATIVE "${ROOT}/game" "${FILE}")
 get_filename_component(DIRECTORY "${RELATIVE}" DIRECTORY)
 target_sources(PixiNativeApp PRIVATE "${FILE}")
 set_source_files_properties("${FILE}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/game/${DIRECTORY}")
endforeach()
target_link_libraries(PixiNativeApp PRIVATE spine_cpp z
 "-framework Foundation" "-framework QuartzCore" "-framework JavaScriptCore"
 "-framework ImageIO" "-framework CoreGraphics" "-framework CoreText" "-framework AVFoundation")

set(ANGLE_ROOT "" CACHE PATH "ANGLE install root containing include and lib directories")
find_path(ANGLE_INCLUDE_DIR EGL/egl.h HINTS "${ANGLE_ROOT}/include" REQUIRED)
find_library(ANGLE_EGL EGL HINTS "${ANGLE_ROOT}/lib" REQUIRED)
find_library(ANGLE_GLES GLESv2 HINTS "${ANGLE_ROOT}/lib" REQUIRED)
target_include_directories(PixiNativeApp PRIVATE "${ANGLE_INCLUDE_DIR}")
target_link_libraries(PixiNativeApp PRIVATE "${ANGLE_EGL}" "${ANGLE_GLES}" "-framework Cocoa")
set_target_properties(PixiNativeApp PROPERTIES BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "@executable_path/../Frameworks")
add_custom_command(TARGET PixiNativeApp POST_BUILD
 COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:PixiNativeApp>/Contents/Frameworks"
 COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ANGLE_EGL}" "${ANGLE_GLES}" "$<TARGET_BUNDLE_DIR:PixiNativeApp>/Contents/Frameworks")
