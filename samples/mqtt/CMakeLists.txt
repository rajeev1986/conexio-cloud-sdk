cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(conexio_mqtt)

# ── Source files ──────────────────────────────────────────────────────────────
target_sources(app PRIVATE
  src/main.c
  src/aws_certs.c
)

# ── Embed TLS certificates as binary includes ─────────────────────────────────
# Place your certificates in src/certs/ before building.
# See README.md for instructions.

set(CERT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/certs)

generate_inc_file_for_target(app
  ${CERT_DIR}/AmazonRootCA1.pem
  ${ZEPHYR_BINARY_DIR}/include/generated/aws_root_ca.pem.inc
)

generate_inc_file_for_target(app
  ${CERT_DIR}/device.crt
  ${ZEPHYR_BINARY_DIR}/include/generated/aws_device_cert.pem.inc
)

generate_inc_file_for_target(app
  ${CERT_DIR}/device.key
  ${ZEPHYR_BINARY_DIR}/include/generated/aws_device_key.pem.inc
)
