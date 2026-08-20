cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app)

# Application sources
target_sources(app PRIVATE
  src/main.c
)

# ── cJSON include path ────────────────────────────────────────────────────────
target_include_directories(app PRIVATE
  ${ZEPHYR_CJSON_MODULE_DIR}
)

# ── nPM1300 fuel gauge ────────────────────────────────────────────────────────
# Compile fuel_gauge.c from the conexio_stratus npm1300 sample and make its
# battery_model.inc (battery lookup table) visible to the compiler.
set(FUEL_GAUGE_SAMPLE_DIR
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../../conexio-firmware-sdk/samples/conexio_stratus/npm1300_fuel_gauge/src
)

target_sources(app PRIVATE ${FUEL_GAUGE_SAMPLE_DIR}/fuel_gauge.c)

# battery_model.inc is #include'd directly by fuel_gauge.c — it must be
# on the compiler search path as a plain include, not a target_sources entry.
target_include_directories(app PRIVATE ${FUEL_GAUGE_SAMPLE_DIR})

# ── Embed AWS Root CA as a binary include ─────────────────────────────────────
set(CERT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/certs)

generate_inc_file_for_target(app
  ${CERT_DIR}/AmazonRootCA1.pem
  ${ZEPHYR_BINARY_DIR}/include/generated/aws_root_ca.pem.inc
)
