cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)
project(${_name} VERSION ${_version} LANGUAGES CXX)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE
  src/plugin-main.cpp
  src/media-library.cpp
  src/media-library.hpp)
find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_17)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
  PLUGIN_NAME="${CMAKE_PROJECT_NAME}"
  PLUGIN_VERSION="${CMAKE_PROJECT_VERSION}")
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES PREFIX "")
if(MSVC)
  target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE /W4 /permissive- /EHsc)
endif()

install(TARGETS ${CMAKE_PROJECT_NAME}
  RUNTIME DESTINATION "mixed-media-slideshow/bin/64bit"
  LIBRARY DESTINATION "mixed-media-slideshow/bin/64bit")
install(FILES data/locale/en-US.ini
  DESTINATION "mixed-media-slideshow/data/locale")

include(CTest)
if(BUILD_TESTING)
  add_executable(media-library-tests tests/media-library-tests.cpp src/media-library.cpp)
  target_include_directories(media-library-tests PRIVATE src)
  target_compile_features(media-library-tests PRIVATE cxx_std_17)
  add_test(NAME media-library-tests COMMAND media-library-tests)
endif()
