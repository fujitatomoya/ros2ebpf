cmake_minimum_required(VERSION 3.16)
project(ros2ebpf_exporter VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBBPF REQUIRED IMPORTED_TARGET libbpf>=1.0)

add_library(ros2ebpf_exporter
  src/buf.c
  src/maps_reader.c
  src/render.c
  src/http.c
  src/hostinfo.c
  src/expectations.c)
target_include_directories(ros2ebpf_exporter
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(ros2ebpf_exporter PUBLIC PkgConfig::LIBBPF)
set_target_properties(ros2ebpf_exporter PROPERTIES
  VERSION ${PROJECT_VERSION} SOVERSION 0 POSITION_INDEPENDENT_CODE ON)

add_executable(ros2ebpf-exporter src/exporter_main.c)
target_link_libraries(ros2ebpf-exporter PRIVATE ros2ebpf_exporter)

option(ROS2EBPF_BUILD_TESTING "Build unit tests" ON)
if(ROS2EBPF_BUILD_TESTING)
  enable_testing()
  add_executable(test_render test/test_render.c)
  target_link_libraries(test_render PRIVATE ros2ebpf_exporter)
  add_test(NAME test_render COMMAND test_render)
endif()

install(TARGETS ros2ebpf_exporter ros2ebpf-exporter
  EXPORT ros2ebpf_exporterTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT ros2ebpf_exporterTargets
  NAMESPACE ros2ebpf::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ros2ebpf_exporter)
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/ros2ebpf_exporterConfigVersion.cmake
  COMPATIBILITY SameMajorVersion)
configure_file(cmake/ros2ebpf_exporterConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/ros2ebpf_exporterConfig.cmake @ONLY)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/ros2ebpf_exporterConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/ros2ebpf_exporterConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ros2ebpf_exporter)

# ament index entry so `colcon build` in a ROS workspace treats this as a package
install(FILES package.xml DESTINATION share/${PROJECT_NAME})
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ament_index_marker "")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ament_index_marker
  DESTINATION share/ament_index/resource_index/packages RENAME ${PROJECT_NAME})

# ---- Debian package (cpack -G DEB) ----------------------------------------
set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME ros2ebpf-exporter)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT "Tomoya Fujita <fujita.tomoya@triorb.co.jp>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "ros2ebpf exporter library, headers and standalone ros2ebpf-exporter")
set(CPACK_DEBIAN_PACKAGE_SECTION libs)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libbpf1 (>= 1:1.0), libelf1t64 | libelf1, zlib1g, libc6")
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_SET_DESTDIR ON)
include(CPack)
