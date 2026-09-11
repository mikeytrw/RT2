cmake_minimum_required(VERSION 3.24)
project(RT2BulletPinballProbe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

# The spike deliberately builds only the mature Bullet 2 rigid-body core.
# Everything unrelated to RT2's pinball slice stays disabled.
set(BUILD_BULLET2_DEMOS OFF CACHE BOOL "" FORCE)
set(BUILD_BULLET3 OFF CACHE BOOL "" FORCE)
set(BUILD_CPU_DEMOS OFF CACHE BOOL "" FORCE)
set(BUILD_ENET OFF CACHE BOOL "" FORCE)
set(BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(BUILD_OPENGL3_DEMOS OFF CACHE BOOL "" FORCE)
set(BUILD_PYBULLET OFF CACHE BOOL "" FORCE)
set(BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(INSTALL_CMAKE_FILES OFF CACHE BOOL "" FORCE)
set(INSTALL_EXTRA_LIBS OFF CACHE BOOL "" FORCE)
set(INSTALL_LIBS OFF CACHE BOOL "" FORCE)
set(USE_DOUBLE_PRECISION OFF CACHE BOOL "" FORCE)
set(USE_GRAPHICAL_BENCHMARK OFF CACHE BOOL "" FORCE)
set(USE_MSVC_RUNTIME_LIBRARY_DLL ON CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
    bullet
    GIT_REPOSITORY https://github.com/bulletphysics/bullet3.git
    GIT_TAG 2c204c49e56ed15ec5fcfa71d199ab6d6570b3f5 # tag 3.25
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(bullet)

add_executable(RT2BulletPinballProbe main.cpp)
target_include_directories(RT2BulletPinballProbe PRIVATE "${bullet_SOURCE_DIR}/src")
target_link_libraries(RT2BulletPinballProbe PRIVATE BulletDynamics BulletCollision LinearMath)

if(MSVC)
    target_compile_options(RT2BulletPinballProbe PRIVATE /W4 /permissive-)
else()
    target_compile_options(RT2BulletPinballProbe PRIVATE -Wall -Wextra -Wpedantic)
endif()
