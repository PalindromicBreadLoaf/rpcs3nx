set(RPCS3_SWITCH_SHELL_VERSION "0.5.0")

execute_process(
	COMMAND git -C "${PROJECT_SOURCE_DIR}" rev-parse --short=12 HEAD
	OUTPUT_VARIABLE RPCS3_SWITCH_REVISION
	OUTPUT_STRIP_TRAILING_WHITESPACE
	ERROR_QUIET
)
if(NOT RPCS3_SWITCH_REVISION)
	set(RPCS3_SWITCH_REVISION "unknown")
endif()

configure_file(build_info.h.in generated/build_info.h @ONLY)

add_executable(rpcs3_switch
	${PROJECT_SOURCE_DIR}/switch/runtime/exception_entry.S
	callback_queue.cpp
	main.cpp
	switch_application.cpp
)
target_include_directories(rpcs3_switch PRIVATE
	"${PROJECT_SOURCE_DIR}"
	"${PROJECT_SOURCE_DIR}/rpcs3"
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_compile_definitions(rpcs3_switch PRIVATE
	_POSIX_C_SOURCE=200809L
	RPCS3_SWITCH_HEAP_SIZE_MB=${RPCS3_SWITCH_HEAP_SIZE_MB}
	RPCS3_SWITCH_JIT_SIZE_MB=${RPCS3_SWITCH_JIT_SIZE_MB}
)
target_compile_options(rpcs3_switch PRIVATE
	-Wall
	-Wextra
	-Wpedantic
	-Werror=return-type
)
target_link_libraries(rpcs3_switch PRIVATE rpcs3_switch_runtime)

nx_generate_nacp(
	OUTPUT rpcs3_switch.nacp
	NAME "RPCS3"
	AUTHOR "RPCS3"
	VERSION "${RPCS3_SWITCH_SHELL_VERSION}"
)
nx_create_nro(
	rpcs3_switch_nro
	TARGET rpcs3_switch
	OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/rpcs3_switch.nro"
	NACP "${CMAKE_CURRENT_BINARY_DIR}/rpcs3_switch.nacp"
)
