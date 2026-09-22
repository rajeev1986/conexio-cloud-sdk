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

# ── Root CA cert ──────────────────────────────────────────────────────────────
# The Conexio Root CA (ConexioRootCA1.pem) is now embedded by the SDK itself.
# It lives in sdk/src/certs/ and is generated into conexio_root_ca.pem.inc
# by the SDK CMakeLists.txt — no entry needed here.
