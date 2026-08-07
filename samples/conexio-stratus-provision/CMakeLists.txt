cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(conexio_stratus_provision)

target_sources(app PRIVATE
  src/main.c
  src/provision.c
  src/cert_store.c
)

# ── Embed bootstrap (claim) credentials ───────────────────────────────────────
# Place these three files in src/certs/ before building:
#   AmazonRootCA1.pem   — AWS root CA (same for all regions)
#   claim.crt           — shared bootstrap certificate
#   claim.key           — shared bootstrap private key
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
