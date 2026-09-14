if(NOT CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch" OR NOT NINTENDO_SWITCH)
	message(FATAL_ERROR "The Switch build requires devkitPro's Switch.cmake toolchain")
endif()

if(NOT CMAKE_CROSSCOMPILING)
	message(FATAL_ERROR "The Switch target must be configured as a cross-build")
endif()

# Switch.cmake supplies -march=armv8-a+crc+crypto, Cortex-A57 tuning, soft TLS,
# PIE, and section-GC conventions.
set(WITH_LLVM OFF CACHE BOOL "LLVM is not integrated with the Horizon shell yet" FORCE)
set(USE_NATIVE_INSTRUCTIONS OFF CACHE BOOL "Native detection cannot run while cross-compiling" FORCE)
set(USE_LTO OFF CACHE BOOL "LTO is disabled for initial Horizon bring-up" FORCE)
set(USE_OPENGL OFF CACHE BOOL "OpenGL is excluded from the initial Horizon build" FORCE)
set(USE_VULKAN OFF CACHE BOOL "Vulkan is integrated in a later milestone" FORCE)

set(RPCS3_SWITCH_HEAP_SIZE_MB 2560 CACHE STRING
	"Newlib heap size in MiB")
if(NOT RPCS3_SWITCH_HEAP_SIZE_MB MATCHES "^[0-9]+$")
	message(FATAL_ERROR "RPCS3_SWITCH_HEAP_SIZE_MB must be an integer number of MiB")
endif()

message(STATUS "Configuring explicit NintendoSwitch application shell")
message(STATUS "Switch newlib heap: ${RPCS3_SWITCH_HEAP_SIZE_MB} MiB")
