cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(fota)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ${ZEPHYR_CJSON_MODULE_DIR})

set(CERT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/certs)
generate_inc_file_for_target(app
  ${CERT_DIR}/AmazonRootCA1.pem
  ${ZEPHYR_BINARY_DIR}/include/generated/aws_root_ca.pem.inc
)
