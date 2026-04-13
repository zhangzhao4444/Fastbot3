@echo off
setlocal

REM 设置 NDK 路径
set NDK_TOOLCHAIN=D:\Android\Sdk\ndk\25.2.9519653\build\cmake\android.toolchain.cmake

REM 可选：清理旧构建
if exist CMakeFiles rmdir /s /q CMakeFiles
if exist CMakeCache.txt del /q CMakeCache.txt

REM 编译 armeabi-v7a
echo ==== Building armeabi-v7a ====
cmake -DCMAKE_TOOLCHAIN_FILE="%NDK_TOOLCHAIN%" -DANDROID_ABI=armeabi-v7a -DCMAKE_BUILD_TYPE=Release .
nmake
if exist CMakeFiles rmdir /s /q CMakeFiles

REM 编译 arm64-v8a
echo ==== Building arm64-v8a ====
cmake -DCMAKE_TOOLCHAIN_FILE="%NDK_TOOLCHAIN%" -DANDROID_ABI=arm64-v8a -DCMAKE_BUILD_TYPE=Release .
nmake
if exist CMakeFiles rmdir /s /q CMakeFiles

REM 编译 x86
echo ==== Building x86 ====
cmake -DCMAKE_TOOLCHAIN_FILE="%NDK_TOOLCHAIN%" -DANDROID_ABI=x86 -DCMAKE_BUILD_TYPE=Release .
nmake
if exist CMakeFiles rmdir /s /q CMakeFiles

REM 编译 x86_64
echo ==== Building x86_64 ====
cmake -DCMAKE_TOOLCHAIN_FILE="%NDK_TOOLCHAIN%" -DANDROID_ABI=x86_64 -DCMAKE_BUILD_TYPE=Release .
nmake
if exist CMakeFiles rmdir /s /q CMakeFiles

echo ==== All architectures built ====
endlocal
