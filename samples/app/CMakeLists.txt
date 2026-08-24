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
# fuel_gauge.c, fuel_gauge.h, and battery_model.inc are copied from
# conexio_stratus/npm1300_fuel_gauge/src into this app's src/ directory.
# battery_model.inc is #include'd directly by fuel_gauge.c so it only needs
# to be on the same search path as fuel_gauge.c — no extra include dir needed.
target_sources(app PRIVATE src/fuel_gauge.c)

# ── Cellular location (AWS Location Service) ──────────────────────────────────
# Collects AT%NCELLMEAS data and publishes it as telemetry metrics.
# The cloud Lambda resolves cell data to coordinates and updates the
# AWS Location Tracker. Enabled via CONFIG_CELL_LOCATION=y in prj.conf.
if(CONFIG_CELL_LOCATION)
  target_sources(app PRIVATE src/cell_location.c)
endif()

# ── Embed AWS Root CA as a binary include ─────────────────────────────────────
set(CERT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/certs)

generate_inc_file_for_target(app
  ${CERT_DIR}/AmazonRootCA1.pem
  ${ZEPHYR_BINARY_DIR}/include/generated/aws_root_ca.pem.inc
)
