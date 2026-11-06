cmake_minimum_required(VERSION 3.16)
project(ros2ebpf_interfaces VERSION 0.1.0 LANGUAGES NONE)

find_package(ament_cmake REQUIRED)

install(FILES README.md request.schema.json status.schema.json
  DESTINATION share/${PROJECT_NAME})

ament_package()
