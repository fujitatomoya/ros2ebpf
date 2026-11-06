cmake_minimum_required(VERSION 3.16)
project(ros2ebpf_daemon VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-unused-result -D_GNU_SOURCE)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBBPF REQUIRED IMPORTED_TARGET libbpf>=1.0)
pkg_check_modules(YAML REQUIRED IMPORTED_TARGET yaml-0.1)
pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)

# The exporter library: installed package, or the sibling source tree.
find_package(ros2ebpf_exporter QUIET)
if(NOT ros2ebpf_exporter_FOUND)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../ros2ebpf_exporter/CMakeLists.txt)
    message(STATUS "Using sibling ros2ebpf_exporter sources")
    set(ROS2EBPF_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../ros2ebpf_exporter ros2ebpf_exporter)
    add_library(ros2ebpf::ros2ebpf_exporter ALIAS ros2ebpf_exporter)
    set(EXPORTER_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../ros2ebpf_exporter/include)
  else()
    message(FATAL_ERROR "ros2ebpf_exporter not found: install it or build from the repository root")
  endif()
else()
  get_target_property(EXPORTER_INCLUDE ros2ebpf::ros2ebpf_exporter INTERFACE_INCLUDE_DIRECTORIES)
endif()

# ---- BPF objects + skeletons ------------------------------------------------
find_program(BPF_CLANG NAMES clang clang-18 clang-17 clang-16 clang-15 REQUIRED)
find_program(BPFTOOL NAMES bpftool REQUIRED)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
  set(BPF_ARCH x86)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(BPF_ARCH arm64)
else()
  message(FATAL_ERROR "unsupported CMAKE_SYSTEM_PROCESSOR ${CMAKE_SYSTEM_PROCESSOR} (x86_64 and aarch64 only)")
endif()
set(BPF_TARGET_ARCH ${BPF_ARCH} CACHE STRING "BPF target arch: x86 or arm64 (override for cross builds)")

set(BPF_DIR ${CMAKE_CURRENT_SOURCE_DIR}/bpf)
set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN_DIR})

foreach(prog rmw_probes udp_drops)
  add_custom_command(
    OUTPUT ${GEN_DIR}/${prog}.bpf.o
    COMMAND ${BPF_CLANG} -g -O2 -target bpf -D__TARGET_ARCH_${BPF_TARGET_ARCH}
            -I${BPF_DIR} -I${EXPORTER_INCLUDE} ${LIBBPF_CFLAGS}
            -Wall -Wno-unused-function
            -c ${BPF_DIR}/${prog}.bpf.c -o ${GEN_DIR}/${prog}.bpf.o
    DEPENDS ${BPF_DIR}/${prog}.bpf.c ${BPF_DIR}/vmlinux_min.h ${EXPORTER_INCLUDE}/ros2ebpf/maps.h
    COMMENT "clang -target bpf ${prog}.bpf.c"
    VERBATIM)
  add_custom_command(
    OUTPUT ${GEN_DIR}/${prog}.skel.h
    COMMAND ${BPFTOOL} gen skeleton ${GEN_DIR}/${prog}.bpf.o > ${GEN_DIR}/${prog}.skel.h
    DEPENDS ${GEN_DIR}/${prog}.bpf.o
    COMMENT "bpftool gen skeleton ${prog}"
    VERBATIM)
  list(APPEND SKELETONS ${GEN_DIR}/${prog}.skel.h)
endforeach()
add_custom_target(bpf_skeletons DEPENDS ${SKELETONS})

# ---- daemon ---------------------------------------------------------------
add_executable(ros2ebpfd
  src/main.c src/config.c src/offsets.c src/probes.c src/identity.c src/udpmap.c src/control.c)
add_dependencies(ros2ebpfd bpf_skeletons)
target_include_directories(ros2ebpfd PRIVATE ${GEN_DIR} src)
target_compile_definitions(ros2ebpfd PRIVATE ROS2EBPFD_VERSION="${PROJECT_VERSION}")
target_link_libraries(ros2ebpfd PRIVATE ros2ebpf::ros2ebpf_exporter PkgConfig::LIBBPF PkgConfig::YAML PkgConfig::JSONC)

add_executable(ros2ebpf-ctl src/ros2ebpf-ctl.c)
target_link_libraries(ros2ebpf-ctl PRIVATE PkgConfig::JSONC)

# ---- tests ----------------------------------------------------------------
option(ROS2EBPF_DAEMON_TESTING "Build daemon unit tests" ON)
if(ROS2EBPF_DAEMON_TESTING)
  enable_testing()
  add_executable(test_config test/test_config.c src/config.c src/offsets.c)
  target_include_directories(test_config PRIVATE src ${EXPORTER_INCLUDE})
  target_link_libraries(test_config PRIVATE PkgConfig::YAML)
  add_test(NAME test_config COMMAND test_config ${CMAKE_CURRENT_SOURCE_DIR}/test)
endif()

# ---- install --------------------------------------------------------------
install(TARGETS ros2ebpfd RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
install(TARGETS ros2ebpf-ctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES systemd/ros2ebpfd.service DESTINATION lib/systemd/system)
# with CMAKE_INSTALL_PREFIX=/usr the unit lands in /usr/lib/systemd/system and ExecStart matches /usr/sbin
# absolute: /etc/ros2ebpf with prefix /usr (packages), /usr/local/etc/ros2ebpf with the default prefix
install(FILES config/config.example.yaml config/expectations.example.yaml
  DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/ros2ebpf)
install(PROGRAMS offsets/gen.sh DESTINATION ${CMAKE_INSTALL_DATADIR}/ros2ebpf/offsets)
install(FILES offsets/README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/ros2ebpf/offsets)
install(FILES ${GEN_DIR}/rmw_probes.bpf.o ${GEN_DIR}/udp_drops.bpf.o
  DESTINATION ${CMAKE_INSTALL_DATADIR}/ros2ebpf/bpf)
install(FILES package.xml DESTINATION share/${PROJECT_NAME})
# Keep colcon from crawling this build tree: cpack stages a copy of package.xml under
# _CPack_Packages/, which otherwise shows up as a duplicate package when the build
# directory lives inside a colcon workspace (fujitatomoya/ros2ebpf#1).
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/COLCON_IGNORE "")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ament_index_marker "")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ament_index_marker
  DESTINATION share/ament_index/resource_index/packages RENAME ${PROJECT_NAME})

# ---- Debian package (cpack -G DEB) ----------------------------------------
set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME ros2ebpf-daemon)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT "Tomoya Fujita <fujita.tomoya@triorb.co.jp>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "ROS 2 endpoint statistics via eBPF: ros2ebpfd, ros2ebpf-ctl, exporter")
set(CPACK_DEBIAN_PACKAGE_SECTION admin)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libbpf1 (>= 1:1.0), libelf1t64 | libelf1, zlib1g, libyaml-0-2, libjson-c5, libc6")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_CURRENT_SOURCE_DIR}/debian/postinst;${CMAKE_CURRENT_SOURCE_DIR}/debian/prerm")
set(CPACK_DEBIAN_PACKAGE_CONFFILES "")
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_SET_DESTDIR ON)
include(CPack)
