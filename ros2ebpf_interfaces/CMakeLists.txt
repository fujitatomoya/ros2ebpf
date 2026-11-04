cmake_minimum_required(VERSION 3.16)
project(ros2ebpf_interfaces VERSION 0.1.0 LANGUAGES NONE)
include(GNUInstallDirs)
install(FILES README.md request.schema.json status.schema.json
  DESTINATION ${CMAKE_INSTALL_DATADIR}/ros2ebpf/interfaces)
install(FILES package.xml DESTINATION share/${PROJECT_NAME})
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ament_index_marker "")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ament_index_marker
  DESTINATION share/ament_index/resource_index/packages RENAME ${PROJECT_NAME})
