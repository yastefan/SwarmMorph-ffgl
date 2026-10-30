# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.29)  # Alembic needs 3.29
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
# Alembic (.abc) support: Alembic and Imath are fetched and linked statically,
# Ogawa back end only (no HDF5).
# ---------------------------------------------------------------------------
option(SWARMMORPH_WITH_ALEMBIC "Support Alembic (.abc) files" ON)
if(SWARMMORPH_WITH_ALEMBIC)
    include(FetchContent)
    # Imath declares an older CMake minimum; without this it would ignore
    # CMAKE_MSVC_RUNTIME_LIBRARY and link the dynamic CRT on Windows.
    set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(IMATH_INSTALL_PKG_CONFIG OFF CACHE BOOL "" FORCE)
    set(ALEMBIC_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(ALEMBIC_DEBUG_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
    set(USE_BINARIES OFF CACHE BOOL "" FORCE)
    set(USE_TESTS OFF CACHE BOOL "" FORCE)
    set(USE_HDF5 OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(Imath
        URL https://github.com/AcademySoftwareFoundation/Imath/archive/refs/tags/v3.2.3.tar.gz
        URL_HASH SHA256=e10c12b3f21f45bf08e09d4215d9c7691368d747beebd840de0b6fefed2df9f8)
    FetchContent_Declare(Alembic
        URL https://github.com/alembic/alembic/archive/refs/tags/1.8.12.tar.gz
        URL_HASH SHA256=b6d916c40446e8c502c84273092ab3d98f3f7f6094f8a2b8203d23e2f1d2a4a0)
    FetchContent_MakeAvailable(Imath)
    # Alembic includes <Imath/half.h> as in an installed tree; Imath's build
    # tree only exposes src/Imath, so add its parent.
    target_include_directories(Imath INTERFACE $<BUILD_INTERFACE:${imath_SOURCE_DIR}/src>)
    FetchContent_MakeAvailable(Alembic)
endif()

# ---------------------------------------------------------------------------
# Mesh loading and sampling, free of OpenGL so the test harness can use it too.
# ---------------------------------------------------------------------------
add_library(swarmmorph-mesh STATIC
    src/MeshSampler.h
    src/MeshSampler.cpp
    src/Shapes.h
    src/Shapes.cpp
    src/AlembicLoader.h
    src/AlembicLoader.cpp
)
target_include_directories(swarmmorph-mesh PUBLIC src)
if(SWARMMORPH_WITH_ALEMBIC)
    target_link_libraries(swarmmorph-mesh PRIVATE Alembic::Alembic)
    target_compile_definitions(swarmmorph-mesh PUBLIC SWARMMORPH_WITH_ALEMBIC=1)
endif()
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
    src/CameraPaths.h
    src/CameraPaths.cpp
    src/Presets.h
    src/Presets.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/PresetData.cpp
    ${FFGL_DIR}/source/lib/FFGLSDK.cpp
)
target_include_directories(SwarmMorph PRIVATE src ${FFGL_DIR}/source/lib)
# The Resolume presets in presets/ are built in; the plugin installs them.
file(GLOB SWARMMORPH_PRESETS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/presets/*.xml)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/PresetData.cpp
    COMMAND ${CMAKE_COMMAND} -DPRESET_DIR=${CMAKE_CURRENT_SOURCE_DIR}/presets
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/PresetData.cpp -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedPresets.cmake
    DEPENDS ${SWARMMORPH_PRESETS} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedPresets.cmake
    COMMENT "Embedding presets")
# Imath sets a global "_d" debug postfix; the host and harness expect the plain name.
set_target_properties(SwarmMorph PROPERTIES DEBUG_POSTFIX "")
target_link_libraries(SwarmMorph PRIVATE swarmmorph-mesh)
# The plugin reports the project version to the host.
target_compile_definitions(SwarmMorph PRIVATE
    SWARMMORPH_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    SWARMMORPH_VERSION_MINOR=${PROJECT_VERSION_MINOR})
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
    target_link_libraries(SwarmMorph PRIVATE ${GLEW_DIR}/lib/Release/x64/glew32s.lib opengl32 shell32 ole32)
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
