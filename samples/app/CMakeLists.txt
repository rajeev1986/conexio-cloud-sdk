cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app)

# Application only provides main.c — all SDK modules are compiled by the
# SDK Zephyr module CMakeLists.txt based on Kconfig selections.
target_sources(app PRIVATE
  src/main.c
)

# ── cJSON include path ────────────────────────────────────────────────────────
# CONFIG_CONEXIO_CLOUD selects CONFIG_CJSON_LIB which builds libcjson.a via
# cmake-ext and links it automatically. Only the include path is needed here.
target_include_directories(app PRIVATE
  ${ZEPHYR_CJSON_MODULE_DIR}
)

# ── Embed TLS certificates as binary includes ─────────────────────────────────
# Place AmazonRootCA1.pem, device.crt, and device.key in src/certs/ before
# building. cert_store.c embeds them via #include at compile time.
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
