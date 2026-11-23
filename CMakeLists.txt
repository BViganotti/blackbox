cmake_minimum_required(VERSION 3.20)
project(blackbox VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(BLACKBOX_BUILD_SHARED "Build the library as a shared library" OFF)
option(BLACKBOX_BUILD_TESTS "Build the test suite" ON)
option(BLACKBOX_BUILD_BENCH "Build benchmarks" ON)
option(BLACKBOX_BUILD_FUZZ "Build fuzz harnesses" ON)
set(BLACKBOX_SANITIZE "" CACHE STRING "Sanitizer: address, undefined or thread")

find_package(Threads REQUIRED)

add_library(blackbox_options INTERFACE)
target_compile_options(blackbox_options INTERFACE
  $<BUILD_INTERFACE:-Wall>
  $<BUILD_INTERFACE:-Wextra>
  $<BUILD_INTERFACE:-Wpedantic>
  $<BUILD_INTERFACE:-Wshadow>
  $<BUILD_INTERFACE:-Werror>
)
target_compile_definitions(blackbox_options INTERFACE
  BLACKBOX_VERSION="${PROJECT_VERSION}"
)
if(APPLE)
  target_compile_definitions(blackbox_options INTERFACE BLACKBOX_PLATFORM_MACOS=1)
elseif(UNIX)
  target_compile_definitions(blackbox_options INTERFACE BLACKBOX_PLATFORM_POSIX=1)
endif()

set(BLACKBOX_SAN_FLAGS "")
if(BLACKBOX_SANITIZE STREQUAL "address")
  set(BLACKBOX_SAN_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_compile_definitions(blackbox_options INTERFACE BLACKBOX_ASAN=1)
elseif(BLACKBOX_SANITIZE STREQUAL "undefined")
  set(BLACKBOX_SAN_FLAGS -fsanitize=undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_compile_definitions(blackbox_options INTERFACE BLACKBOX_UBSAN=1)
elseif(BLACKBOX_SANITIZE STREQUAL "thread")
  set(BLACKBOX_SAN_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
  target_compile_definitions(blackbox_options INTERFACE BLACKBOX_TSAN=1)
elseif(NOT BLACKBOX_SANITIZE STREQUAL "")
  message(FATAL_ERROR "Unknown BLACKBOX_SANITIZE=${BLACKBOX_SANITIZE}")
endif()
if(NOT BLACKBOX_SAN_FLAGS STREQUAL "")
  set(BLACKBOX_SAN_BUILD_FLAGS "")
  foreach(flag IN LISTS BLACKBOX_SAN_FLAGS)
    list(APPEND BLACKBOX_SAN_BUILD_FLAGS "$<BUILD_INTERFACE:${flag}>")
  endforeach()
  target_compile_options(blackbox_options INTERFACE ${BLACKBOX_SAN_BUILD_FLAGS})
  target_link_options(blackbox_options INTERFACE ${BLACKBOX_SAN_BUILD_FLAGS})
endif()

file(GLOB BLACKBOX_LIB_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/demo/*.cpp"
)
set(BLACKBOX_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(BLACKBOX_ASSETS_CPP "${BLACKBOX_GENERATED_DIR}/assets.cpp")
file(MAKE_DIRECTORY "${BLACKBOX_GENERATED_DIR}")
set(BLACKBOX_ASSET_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/ui/index.html"
  "${CMAKE_CURRENT_SOURCE_DIR}/ui/app.css"
  "${CMAKE_CURRENT_SOURCE_DIR}/ui/app.js"
)
add_custom_command(
  OUTPUT "${BLACKBOX_ASSETS_CPP}"
  COMMAND "${CMAKE_COMMAND}"
    -DBLACKBOX_ASSET_DIR=${CMAKE_CURRENT_SOURCE_DIR}/ui
    -DBLACKBOX_OUTPUT=${BLACKBOX_ASSETS_CPP}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_assets.cmake"
  DEPENDS ${BLACKBOX_ASSET_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_assets.cmake"
  COMMENT "Embedding Blackbox UI assets"
  VERBATIM
)
set_source_files_properties("${BLACKBOX_ASSETS_CPP}" PROPERTIES GENERATED TRUE)
list(APPEND BLACKBOX_LIB_SOURCES "${BLACKBOX_ASSETS_CPP}")

if(BLACKBOX_BUILD_SHARED)
  add_library(blackbox SHARED ${BLACKBOX_LIB_SOURCES})
else()
  add_library(blackbox STATIC ${BLACKBOX_LIB_SOURCES})
endif()
set_target_properties(blackbox PROPERTIES
  VERSION "${PROJECT_VERSION}"
  SOVERSION 1
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(blackbox PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(blackbox PUBLIC blackbox_options Threads::Threads)

function(blackbox_add_executable name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE blackbox)
endfunction()

enable_testing()

if(BLACKBOX_BUILD_TESTS)
  file(GLOB BLACKBOX_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
  )
  add_executable(blackbox_tests ${BLACKBOX_TEST_SOURCES})
  target_link_libraries(blackbox_tests PRIVATE blackbox)
  target_include_directories(blackbox_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  add_test(NAME unit COMMAND blackbox_tests)
  add_test(NAME launcher_smoke COMMAND blackbox_launcher --no-browser
           --run-seconds 1 --dump-dir "${CMAKE_CURRENT_BINARY_DIR}/smoke-dumps")
  set_tests_properties(launcher_smoke PROPERTIES TIMEOUT 30)
  add_test(NAME embedding_smoke COMMAND blackbox_embedding)
  set_tests_properties(embedding_smoke PROPERTIES TIMEOUT 30)
  add_test(NAME launcher_version COMMAND blackbox_launcher --version)
  set_tests_properties(launcher_version PROPERTIES
    PASS_REGULAR_EXPRESSION "blackbox")
  add_test(NAME soak_smoke COMMAND blackbox_soak --seconds 2)
  set_tests_properties(soak_smoke PROPERTIES TIMEOUT 30)
endif()

blackbox_add_executable(blackbox_launcher
  "${CMAKE_CURRENT_SOURCE_DIR}/src/app/launcher.cpp")

add_executable(blackbox_demo
  "${CMAKE_CURRENT_SOURCE_DIR}/examples/blackbox_demo/main.cpp")
target_link_libraries(blackbox_demo PRIVATE blackbox)

blackbox_add_executable(blackbox_embedding
  "${CMAKE_CURRENT_SOURCE_DIR}/examples/embedding/main.cpp")

if(BLACKBOX_BUILD_TESTS)
  blackbox_add_executable(blackbox_soak
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/soak_main.cpp")
endif()

if(BLACKBOX_BUILD_BENCH)
  blackbox_add_executable(blackbox_bench "${CMAKE_CURRENT_SOURCE_DIR}/src/app/bench_main.cpp")
endif()

if(BLACKBOX_BUILD_FUZZ)
  blackbox_add_executable(blackbox_fuzz "${CMAKE_CURRENT_SOURCE_DIR}/src/app/fuzz_main.cpp")
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS blackbox blackbox_options
        EXPORT blackboxTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(TARGETS blackbox_launcher RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/blackbox"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT blackboxTargets
        FILE blackboxTargets.cmake
        NAMESPACE bb::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/blackbox")
configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/blackboxConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/blackboxConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/blackbox")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/blackboxConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/blackboxConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/blackboxConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/blackbox")
install(FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
  "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
  "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md"
  "${CMAKE_CURRENT_SOURCE_DIR}/LIMITATIONS.md"
  DESTINATION "${CMAKE_INSTALL_DOCDIR}"
)
