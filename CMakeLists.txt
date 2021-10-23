cmake_minimum_required(VERSION 3.15)

project(socks5)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_EXPORT_COMPILE_COMMANDS "ON")


set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)


include(FetchContent)

FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(asio)

include_directories(${asio_SOURCE_DIR}/asio/include)

find_package(Threads)

add_executable(server server.cpp)
add_executable(local local.cpp)

target_link_libraries(server
    Threads::Threads
    )

target_link_libraries(local
    Threads::Threads
    )
