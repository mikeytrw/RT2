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

# Do not add Bullet's top-level project: even with its demos disabled it adds
# unrelated libraries (soft body, inverse dynamics, Bullet3Common) to the
# default build. Populate the pinned source, then expose only the three
# mature rigid-body modules used by this probe.
FetchContent_GetProperties(bullet)
if(NOT bullet_POPULATED)
    cmake_policy(SET CMP0169 OLD)
    FetchContent_Populate(bullet)
endif()
set(BULLET_PHYSICS_SOURCE_DIR "${bullet_SOURCE_DIR}")
set(BULLET_VERSION 3.25)
set(CMAKE_DEBUG_POSTFIX "_Debug")
add_subdirectory("${bullet_SOURCE_DIR}/src/LinearMath" "${bullet_BINARY_DIR}/src/LinearMath")
add_subdirectory("${bullet_SOURCE_DIR}/src/BulletCollision" "${bullet_BINARY_DIR}/src/BulletCollision")
add_subdirectory("${bullet_SOURCE_DIR}/src/BulletDynamics" "${bullet_BINARY_DIR}/src/BulletDynamics")

add_executable(RT2BulletPinballProbe main.cpp)
target_include_directories(RT2BulletPinballProbe PRIVATE "${bullet_SOURCE_DIR}/src")
target_link_libraries(RT2BulletPinballProbe PRIVATE BulletDynamics BulletCollision LinearMath)

if(MSVC)
    target_compile_options(RT2BulletPinballProbe PRIVATE /W4 /permissive-)
else()
    target_compile_options(RT2BulletPinballProbe PRIVATE -Wall -Wextra -Wpedantic)
endif()
