cmake_minimum_required(VERSION 3.16)
project(ros2ebpf_daemon VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-unused-result -D_GNU_SOURCE)

find_package(ament_cmake REQUIRED)
find_package(ros2ebpf_exporter REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBBPF REQUIRED IMPORTED_TARGET libbpf>=1.0)
pkg_check_modules(YAML REQUIRED IMPORTED_TARGET yaml-0.1)
pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)

# ros2ebpf/maps.h is shared with the BPF programs (compiled below outside of CMake's C toolchain)
set(EXPORTER_INCLUDE ${ros2ebpf_exporter_INCLUDE_DIRS})

# ---- BPF objects + skeletons ------------------------------------------------
find_program(BPF_CLANG NAMES clang clang-21 clang-20 clang-19 clang-18 clang-17 clang-16 clang-15 REQUIRED)
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
target_link_libraries(ros2ebpfd PRIVATE
  ros2ebpf_exporter::ros2ebpf_exporter PkgConfig::LIBBPF PkgConfig::YAML PkgConfig::JSONC)

add_executable(ros2ebpf-ctl src/ros2ebpf-ctl.c)
target_link_libraries(ros2ebpf-ctl PRIVATE PkgConfig::JSONC)

# ---- tests ----------------------------------------------------------------
option(BUILD_TESTING "Build daemon unit tests" ON)
if(BUILD_TESTING)
  find_package(ament_cmake_test REQUIRED)
  add_executable(test_config test/test_config.c src/config.c src/offsets.c)
  target_include_directories(test_config PRIVATE src ${EXPORTER_INCLUDE})
  target_link_libraries(test_config PRIVATE PkgConfig::YAML)
  ament_add_test(test_config
    COMMAND "$<TARGET_FILE:test_config>" ${CMAKE_CURRENT_SOURCE_DIR}/test
    GENERATE_RESULT_FOR_RETURN_CODE_ZERO)
endif()

# ---- install --------------------------------------------------------------
# Executables: bin/ so they are on PATH (systemd unit, containers), lib/<pkg>/ for `ros2 run`.
install(TARGETS ros2ebpfd ros2ebpf-ctl RUNTIME DESTINATION bin)
install(TARGETS ros2ebpfd ros2ebpf-ctl RUNTIME DESTINATION lib/${PROJECT_NAME})

# systemd unit with the absolute daemon path of this install prefix:
# /usr/bin/ros2ebpfd in the Debian package, <workspace>/install/bin/ros2ebpfd for a colcon build.
set(ROS2EBPFD_BIN ${CMAKE_INSTALL_PREFIX}/bin/ros2ebpfd)
configure_file(systemd/ros2ebpfd.service.in ${CMAKE_CURRENT_BINARY_DIR}/ros2ebpfd.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ros2ebpfd.service DESTINATION lib/systemd/system)

# /etc/ros2ebpf with prefix /usr (Debian package), <prefix>/etc/ros2ebpf otherwise (colcon install/, images)
if(CMAKE_INSTALL_PREFIX MATCHES "^/usr/?$")
  set(ROS2EBPF_SYSCONFDIR /etc)
else()
  set(ROS2EBPF_SYSCONFDIR etc)
endif()
install(FILES config/config.example.yaml config/expectations.example.yaml
  DESTINATION ${ROS2EBPF_SYSCONFDIR}/ros2ebpf)
install(PROGRAMS offsets/gen.sh DESTINATION share/${PROJECT_NAME}/offsets)
install(FILES offsets/README.md DESTINATION share/${PROJECT_NAME}/offsets)
install(FILES ${GEN_DIR}/rmw_probes.bpf.o ${GEN_DIR}/udp_drops.bpf.o
  DESTINATION share/${PROJECT_NAME}/bpf)

# Keep colcon from crawling this build tree: cpack stages a copy of package.xml under
# _CPack_Packages/, which otherwise shows up as a duplicate package when the build
# directory lives inside a colcon workspace (fujitatomoya/ros2ebpf#1).
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/COLCON_IGNORE "")

# ---- Debian package (cpack -G DEB, see `make deb`) ------------------------
set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME ros2ebpf-daemon)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT "Tomoya Fujita <fujita.tomoya@triorb.co.jp>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "ROS 2 endpoint statistics via eBPF: ros2ebpfd and ros2ebpf-ctl")
set(CPACK_DEBIAN_PACKAGE_SECTION admin)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libbpf1 (>= 1:1.0), libelf1t64 | libelf1, zlib1g, libyaml-0-2, libjson-c5, libc6")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_CURRENT_SOURCE_DIR}/debian/postinst;${CMAKE_CURRENT_SOURCE_DIR}/debian/prerm")
set(CPACK_DEBIAN_PACKAGE_CONFFILES "")
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_SET_DESTDIR ON)
include(CPack)

ament_package()
