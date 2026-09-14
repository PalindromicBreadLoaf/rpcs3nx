add_library(rpcs3_switch_runtime STATIC
	${PROJECT_SOURCE_DIR}/Utilities/File.cpp
	${PROJECT_SOURCE_DIR}/Utilities/StrFmt.cpp
	${PROJECT_SOURCE_DIR}/Utilities/mutex.cpp
	${PROJECT_SOURCE_DIR}/rpcs3/util/atomic.cpp
	vm_native.cpp
	exception_handler.cpp
	guest_memory.cpp
	jit_memory.cpp
	runtime.cpp
	sysinfo.cpp
	thread.cpp
)
target_include_directories(rpcs3_switch_runtime PUBLIC
	"${PROJECT_SOURCE_DIR}"
	"${PROJECT_SOURCE_DIR}/rpcs3"
)
target_compile_definitions(rpcs3_switch_runtime PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(rpcs3_switch_runtime PRIVATE
	-Wall
	-Wextra
	-Wpedantic
	-Werror=return-type
)
