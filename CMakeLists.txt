cmake_minimum_required(VERSION 3.16)

project(mini_kv_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
	asynclogger
	GIT_REPOSITORY https://github.com/manba-kebi/async_logger.git
	GIT_TAG c8d4fe6e5c691e2f196df8e3bcb3f8309c5cf9db
)

set(ASYNCLOGGER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ASYNCLOGGER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASYNCLOGGER_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(asynclogger)

add_library(minikv					# 创建一个名为 minikv 的静态库（默认类型），由列出的 6 个源文件编译而成。
	src/net/socket_runtime.cpp
	src/net/socket.cpp
	src/core/thread_pool.cpp
	src/core/key_value_store.cpp
	src/core/command.cpp
	src/core/append_only_file.cpp		# 这个AOF文件是后填的，可以用于在崩溃后按顺序回放恢复状态
	src/server/server.cpp
)

target_include_directories(minikv
	PUBLIC
		${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(minikv
	PUBLIC
		asynclogger::asynclogger
)

if(WIN32)
	target_compile_definitions(minikv PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX) # 排除 Windows 头文件中不常用的 API 以加快编译 以及禁止 windows.h 定义 min/max 宏
	target_link_libraries(minikv PRIVATE ws2_32)
endif()

if(MSVC)
	target_compile_options(minikv PRIVATE /W4 /permissive- /utf-8)		# 在 MSVC 编译器下，添加编译选项 /W4（高警告级别）和 /permissive-（更严格的标准一致性模式）
else()
	target_compile_options(minikv PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(mini_kv_server src/main.cpp)
target_link_libraries(mini_kv_server PRIVATE minikv)

if(UNIX AND NOT APPLE)
	add_library(minikv_epoll
		src/server/epoll_server.cpp
	)

	target_link_libraries(minikv_epoll
		PUBLIC
			minikv
	)

	target_compile_options(minikv_epoll PRIVATE -Wall -Wextra -Wpedantic)

	add_executable(mini_kv_server_epoll src/epoll_main.cpp)
	target_link_libraries(mini_kv_server_epoll PRIVATE minikv_epoll)
endif()

enable_testing()

add_executable(command_test tests/command_test.cpp)
target_link_libraries(command_test PRIVATE minikv)
add_test(NAME command_test COMMAND command_test)

add_executable(key_value_store_test tests/key_value_store_test.cpp)
target_link_libraries(key_value_store_test PRIVATE minikv)
add_test(NAME key_value_store_test COMMAND key_value_store_test)