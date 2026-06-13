cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(conexio_fleet_provisioning)

# ── Source files ──────────────────────────────────────────────────────────────
target_sources(app PRIVATE
  src/main.c
  src/provision.c
  src/cert_store.c
  src/uart_commissioning.c
)

# ── Embed the claim certificate (shared across all devices) ───────────────────
# Place claim.crt, claim.key, and AmazonRootCA1.pem in src/certs/ before building.
# These are the BOOTSTRAP credentials — NOT per-device.
# See README.md for instructions on obtaining them from AWS.

set(CERT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/certs)

generate_inc_file_for_target(app
  ${CERT_DIR}/AmazonRootCA1.pem
  ${ZEPHYR_BINARY_DIR}/include/generated/aws_root_ca.pem.inc
)

generate_inc_file_for_target(app
  ${CERT_DIR}/claim.crt
  ${ZEPHYR_BINARY_DIR}/include/generated/claim_cert.pem.inc
)

generate_inc_file_for_target(app
  ${CERT_DIR}/claim.key
  ${ZEPHYR_BINARY_DIR}/include/generated/claim_key.pem.inc
)
