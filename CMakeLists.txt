cmake_minimum_required(VERSION 3.15)

project(spawn)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_EXPORT_COMPILE_COMMANDS "ON")


set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads)

set(Boost_USE_STATIC_LIBS ON)
set(Boost_USE_DEBUG_LIBS ON)
set(Boost_USE_RELEASE_LIBS ON)
set(Boost_USE_MULTITHREADED ON)

find_package(Boost 1.77.0 COMPONENTS system coroutine thread REQUIRED)

include_directories(
    ${Boost_INCLUDE_DIRS}
    )

add_executable(server server.cpp)
add_executable(local local.cpp)

target_link_libraries(server
    ${Boost_LIBRARIES}
    Threads::Threads
    )


target_link_libraries(local
    ${Boost_LIBRARIES}
    Threads::Threads
    )
