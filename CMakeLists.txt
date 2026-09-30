cmake_minimum_required(VERSION 3.15)
project(tcp_demo CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC 下强制按 UTF-8 解析源文件
if(MSVC)
    add_compile_options(/utf-8)
endif()

if(WIN32)
    set(PLATFORM_LIBS ws2_32 mswsock)
else()
    set(PLATFORM_LIBS)
endif()

add_executable(server server/main.cpp)
target_link_libraries(server PRIVATE ${PLATFORM_LIBS})

add_executable(client client/main.cpp)
target_link_libraries(client PRIVATE ${PLATFORM_LIBS})