# The example C ABI plugin on its own (#201): what a plugin author needs is this folder and ../include/shz_abi.h.
#
#   cmake -S . -B build && cmake --build build --config Release
#
# Output: build/cabi_example/ is the package. Copy it to shizotech/packages/ next to shz (or next to the script that
# imports it) and run: import cabi_example; std.print(cabi_example.add(2, 3));

cmake_minimum_required(VERSION 3.16)
project(cabi_example C)

if(WIN32)
	set(SHZ_PLATFORM windows_x64)
elseif(APPLE)
	if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
		set(SHZ_PLATFORM macos_arm64)
	else()
		set(SHZ_PLATFORM macos_x64)
	endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
	set(SHZ_PLATFORM linux_arm64)
else()
	set(SHZ_PLATFORM linux_x64)
endif()

add_library(cabi_example SHARED cabi_example.c)
target_include_directories(cabi_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
# $<0:> keeps multi-config generators (Visual Studio, Xcode) from adding a Release/ folder
set(SHZ_NATIVE_DIR ${CMAKE_BINARY_DIR}/cabi_example/native/${SHZ_PLATFORM}$<0:>)
set_target_properties(cabi_example PROPERTIES
	C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF C_VISIBILITY_PRESET hidden
	PREFIX "" OUTPUT_NAME shzmodule_cabi_example
	LIBRARY_OUTPUT_DIRECTORY ${SHZ_NATIVE_DIR}
	RUNTIME_OUTPUT_DIRECTORY ${SHZ_NATIVE_DIR}
)
if(APPLE)
	set_target_properties(cabi_example PROPERTIES SUFFIX .dylib)
elseif(NOT WIN32)
	# nothing of ShizoScript is needed at link time
	target_link_options(cabi_example PRIVATE -Wl,--no-undefined)
endif()
file(WRITE ${CMAKE_BINARY_DIR}/cabi_example/package.shz "//cabi_example: the example plugin of the ShizoScript C ABI\n")
