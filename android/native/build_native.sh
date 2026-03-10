#!/bin/bash
# cd "native"

# Check if NDK_ROOT is set
if [ -z "$NDK_ROOT" ]; then
    echo "Error: NDK_ROOT environment variable is not set."
    echo "Please set it to your Android NDK path, for example:"
    echo "  export NDK_ROOT=\$HOME/Library/Android/sdk/ndk/<version>"
    echo "  or"
    echo "  export NDK_ROOT=\$ANDROID_NDK_HOME"
    exit 1
fi

# Check if the toolchain file exists
TOOLCHAIN_FILE="$NDK_ROOT/build/cmake/android.toolchain.cmake"
if [ ! -f "$TOOLCHAIN_FILE" ]; then
    echo "Error: Android toolchain file not found at: $TOOLCHAIN_FILE"
    echo "Please verify that NDK_ROOT is set correctly and the NDK version supports CMake."
    exit 1
fi

# Function to build for a specific ABI
build_abi() {
    local ABI=$1
    echo "Building for $ABI..."
    
    # Remove CMakeFiles if it exists (ignore error if it doesn't)
    rm -rf CMakeFiles/ CMakeCache.txt cmake_install.cmake Makefile 2>/dev/null
    
    # Configure CMake
    cmake -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
          -DANDROID_ABI="$ABI" \
          -DCMAKE_BUILD_TYPE=Release \
          .
    
    if [ $? -ne 0 ]; then
        echo "Error: CMake configuration failed for $ABI"
        return 1
    fi
    
    # Build (CMake should create all necessary directories)
    make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
    
    if [ $? -ne 0 ]; then
        echo "Error: Build failed for $ABI"
        return 1
    fi
    
    echo "Successfully built for $ABI"
    return 0
}

# Build for all ABIs
build_abi armeabi-v7a
build_abi arm64-v8a
build_abi x86
build_abi x86_64

echo "All builds completed!"
