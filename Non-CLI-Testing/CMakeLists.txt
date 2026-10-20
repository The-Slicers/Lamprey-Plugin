# Builds the JNI bridge between Quorum's Slicer class and OrcaSlicer.
#
#   Windows : QuorumSlicer.dll
#   macOS   : libQuorumSlicer.dylib  (universal arm64 + x86_64)
#   Linux   : libQuorumSlicer.so
#
# Copy the result into the "jni" folder next to the Quorum plugins jar
# (the same folder that holds libGameEngineCPlugins).
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release
#
# Needs a JDK (JAVA_HOME) for the JNI headers. Nothing from OrcaSlicer is
# needed at build time; OrcaSlicer only has to be installed on the machine
# that runs the Quorum program.

cmake_minimum_required(VERSION 3.16)

if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    # std::filesystem needs macOS 10.15 or newer.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "" FORCE)
endif()

project(QuorumSlicer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Only the JNI headers (jni.h, jni_md.h) are needed, not libjvm or AWT, so a
# headless JDK is fine even when FindJNI reports AWT missing.
find_package(JNI)
if(NOT JAVA_INCLUDE_PATH OR NOT JAVA_INCLUDE_PATH2)
    message(FATAL_ERROR "JNI headers not found. Set JAVA_HOME to a JDK.")
endif()

add_library(QuorumSlicer SHARED SlicerJNI.cpp)
target_include_directories(QuorumSlicer PRIVATE ${JAVA_INCLUDE_PATH} ${JAVA_INCLUDE_PATH2})

# Hide everything except the JNIEXPORT functions.
set_target_properties(QuorumSlicer PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

# Match System.mapLibraryName: QuorumSlicer.dll, not libQuorumSlicer.dll (MinGW).
if(WIN32)
    set_target_properties(QuorumSlicer PROPERTIES PREFIX "")
endif()

if(MSVC)
    target_compile_options(QuorumSlicer PRIVATE /W4 /utf-8 /EHsc)
    # Static CRT so users do not need the Visual C++ redistributable.
    set_property(TARGET QuorumSlicer PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(QuorumSlicer PRIVATE -Wall -Wextra)
endif()

if(MINGW)
    # Do not depend on MinGW's runtime DLLs at run time.
    target_link_options(QuorumSlicer PRIVATE -static -static-libgcc -static-libstdc++)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Old GCC (< 9) keeps std::filesystem in a separate library.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
        target_link_libraries(QuorumSlicer PRIVATE stdc++fs)
    endif()
    # Keep libstdc++ out of the runtime dependencies so the .so runs on more distros.
    # --exclude-libs keeps the static libstdc++ symbols private to this library.
    target_link_options(QuorumSlicer PRIVATE -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL)
endif()
