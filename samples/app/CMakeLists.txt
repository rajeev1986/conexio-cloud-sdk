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

# ── Embed AWS Root CA as a binary include ─────────────────────────────────────
# AmazonRootCA1.pem is the TLS trust anchor for AWS IoT Core MQTT.
# It is public, static, and identical for every device — safe to embed.
#
# The device certificate and private key are NOT embedded here.
# They are provisioned into the modem by the conexio-stratus-provision sample
# (fleet provisioning via AT%KEYGEN + CreateCertificateFromCsr) and are
# verified at runtime by cert_store_provision_from_config() in cert_store.c.
# Flash conexio-stratus-provision once per device before flashing this app.
set(CERT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/certs)

generate_inc_file_for_target(app
  ${CERT_DIR}/AmazonRootCA1.pem
  ${ZEPHYR_BINARY_DIR}/include/generated/aws_root_ca.pem.inc
)
