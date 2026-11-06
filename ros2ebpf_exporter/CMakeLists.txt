cmake_minimum_required(VERSION 3.16)
project(ros2ebpf_exporter VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE)

find_package(ament_cmake REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBBPF REQUIRED libbpf>=1.0)

# Static library: the daemon links it in, the standalone exporter and the unit tests too.
# libbpf is linked by absolute path (not through an imported PkgConfig:: target) so that the
# exported target is self-contained for downstream find_package(ros2ebpf_exporter).
add_library(ros2ebpf_exporter STATIC
  src/buf.c
  src/maps_reader.c
  src/render.c
  src/http.c
  src/hostinfo.c
  src/expectations.c)
target_include_directories(ros2ebpf_exporter
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
         ${LIBBPF_INCLUDE_DIRS})
target_compile_options(ros2ebpf_exporter PUBLIC ${LIBBPF_CFLAGS_OTHER})
target_link_libraries(ros2ebpf_exporter PUBLIC ${LIBBPF_LINK_LIBRARIES})
set_target_properties(ros2ebpf_exporter PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(ros2ebpf-exporter src/exporter_main.c)
target_link_libraries(ros2ebpf-exporter PRIVATE ros2ebpf_exporter)

# ---- tests ----------------------------------------------------------------
option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
  find_package(ament_cmake_test REQUIRED)
  add_executable(test_render test/test_render.c)
  target_link_libraries(test_render PRIVATE ros2ebpf_exporter)
  ament_add_test(test_render
    COMMAND "$<TARGET_FILE:test_render>"
    GENERATE_RESULT_FOR_RETURN_CODE_ZERO)
endif()

# ---- install --------------------------------------------------------------
install(TARGETS ros2ebpf_exporter
  EXPORT export_${PROJECT_NAME}
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin)
install(TARGETS ros2ebpf-exporter RUNTIME DESTINATION bin)                   # on PATH
install(TARGETS ros2ebpf-exporter RUNTIME DESTINATION lib/${PROJECT_NAME})   # ros2 run ros2ebpf_exporter ros2ebpf-exporter
install(DIRECTORY include/ DESTINATION include)

ament_export_include_directories(include)
ament_export_targets(export_${PROJECT_NAME} HAS_LIBRARY_TARGET)

# Keep colcon from crawling this build tree (cpack stages package.xml under _CPack_Packages/).
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/COLCON_IGNORE "")

# ---- Debian package (cpack -G DEB, see `make deb`) ------------------------
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

ament_package()
