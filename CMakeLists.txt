cmake_minimum_required(VERSION 3.20)
project(SwarmMorph VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
    # Static CRT so the DLL has no runtime dependencies on the target machine.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(FFGL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/ffgl)
if(NOT EXISTS ${FFGL_DIR}/source/lib/FFGLSDK.cpp)
    message(FATAL_ERROR "FFGL SDK not found. Run: git submodule update --init --recursive")
endif()

# ---------------------------------------------------------------------------
# Mesh loading and sampling, free of OpenGL so the test harness can use it too.
# ---------------------------------------------------------------------------
add_library(swarmmorph-mesh STATIC
    src/MeshSampler.h
    src/MeshSampler.cpp
)
target_include_directories(swarmmorph-mesh PUBLIC src)
if(MSVC)
    target_compile_definitions(swarmmorph-mesh PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ---------------------------------------------------------------------------
# The FFGL plugin: .bundle on macOS, .dll on Windows.
# ---------------------------------------------------------------------------
add_library(SwarmMorph MODULE
    src/SwarmMorph.h
    src/SwarmMorph.cpp
    src/Shaders.h
    src/MathUtil.h
    ${FFGL_DIR}/source/lib/FFGLSDK.cpp
)
target_include_directories(SwarmMorph PRIVATE src ${FFGL_DIR}/source/lib)
target_link_libraries(SwarmMorph PRIVATE swarmmorph-mesh)
# FFGL_DEBUG makes the SDK assert that the plugin leaves the GL context in its default state.
target_compile_definitions(SwarmMorph PRIVATE $<$<CONFIG:Debug>:DEBUG>)

if(APPLE)
    find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)
    target_link_libraries(SwarmMorph PRIVATE ${OPENGL_FRAMEWORK})
    target_compile_definitions(SwarmMorph PRIVATE GL_SILENCE_DEPRECATION)
    set_target_properties(SwarmMorph PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION bundle
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in
        MACOSX_BUNDLE_GUI_IDENTIFIER com.swarmmorph.ffgl
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    )
elseif(WIN32)
    set(GLEW_DIR ${FFGL_DIR}/deps/glew-2.1.0)
    target_include_directories(SwarmMorph PRIVATE ${GLEW_DIR}/include)
    target_compile_definitions(SwarmMorph PRIVATE GLEW_STATIC _CRT_SECURE_NO_WARNINGS NOMINMAX)
    target_link_libraries(SwarmMorph PRIVATE ${GLEW_DIR}/lib/Release/x64/glew32s.lib opengl32)
    set_target_properties(SwarmMorph PROPERTIES PREFIX "")
endif()

# ---------------------------------------------------------------------------
# Offscreen test harness (macOS only for now): loads the built plugin like a
# host would, renders frames and writes PNGs.
# ---------------------------------------------------------------------------
option(SWARMMORPH_BUILD_HARNESS "Build the offscreen test harness" ${APPLE})
if(SWARMMORPH_BUILD_HARNESS AND APPLE)
    enable_language(OBJCXX)
    add_executable(swarmmorph-harness tools/harness/main.mm)
    target_include_directories(swarmmorph-harness PRIVATE ${FFGL_DIR}/source/lib)
    target_compile_definitions(swarmmorph-harness PRIVATE GL_SILENCE_DEPRECATION)
    target_link_libraries(swarmmorph-harness PRIVATE
        "-framework OpenGL" "-framework CoreGraphics" "-framework ImageIO" "-framework CoreServices" "-framework Foundation")
    set_source_files_properties(tools/harness/main.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    add_dependencies(swarmmorph-harness SwarmMorph)
endif()
