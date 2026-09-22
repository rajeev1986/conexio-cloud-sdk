cmake_minimum_required(VERSION 3.20.0)

if(CONFIG_CONEXIO_CLOUD)

  # ── Conexio Root CA — embedded into cert_store.c at build time ──────────────
  # The Root CA is public and identical for every device — safe to embed in the
  # SDK so no sample or application needs to carry or reference it.
  # cert_store.c includes the generated conexio_root_ca.pem.inc directly.
  set(CONEXIO_CERT_DIR ${CMAKE_CURRENT_LIST_DIR}/src/certs)
  generate_inc_file_for_target(app
    ${CONEXIO_CERT_DIR}/ConexioRootCA1.pem
    ${ZEPHYR_BINARY_DIR}/include/generated/conexio_root_ca.pem.inc
  )

  # Public headers — application only needs to #include <conexio_cloud/conexio_cloud.h>
  # and the feature headers (fota.h, offline_buffer.h, power_mgr.h, retry.h)
  target_include_directories(app PUBLIC include src)

  # ── Core SDK ────────────────────────────────────────────────────────────────
  target_sources(app PRIVATE
    src/conexio_cloud.c
    src/config_fetch.c
    src/lte.c
    src/cert_store.c
  )

  # ── Transport (select one) ───────────────────────────────────────────────────
  if(CONFIG_CONEXIO_CLOUD_MQTT)
    target_sources(app PRIVATE src/transport/mqtt_transport.c)
  endif()

  if(CONFIG_CONEXIO_CLOUD_HTTP)
    target_sources(app PRIVATE src/transport/http_transport.c)
  endif()

  # ── FOTA — OTA firmware updates via AWS IoT Jobs + MCUboot ──────────────────
  # Enable with CONFIG_CONEXIO_CLOUD_FOTA=y in prj.conf
  if(CONFIG_CONEXIO_CLOUD_FOTA)
    target_sources(app PRIVATE src/fota.c)
  endif()

  # ── Offline buffer — flash-backed ring buffer for offline telemetry ──────────
  # Enable with CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER=y in prj.conf
  if(CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER)
    target_sources(app PRIVATE src/offline_buffer.c)
  endif()

  # ── Power manager — LTE PSM/eDRX negotiation ─────────────────────────────────
  # Enable with CONFIG_CONEXIO_CLOUD_PSM=y in prj.conf
  if(CONFIG_CONEXIO_CLOUD_PSM)
    target_sources(app PRIVATE src/power_mgr.c)
  endif()

  # ── Retry / watchdog — exponential backoff + hardware watchdog ──────────────
  # Enable with CONFIG_CONEXIO_CLOUD_RETRY=y in prj.conf
  if(CONFIG_CONEXIO_CLOUD_RETRY)
    target_sources(app PRIVATE src/retry.c)
  endif()

  # ── Cellular location — AT%NCELLMEAS → Conexio Location Service ─────────────
  # Enable with CONFIG_CELL_LOCATION=y in prj.conf.
  # Measurement interval controlled by CONFIG_CELL_LOCATION_INTERVAL_SEC.
  if(CONFIG_CELL_LOCATION)
    target_sources(app PRIVATE src/cell_location.c)
  endif()
  # ── nPM13xx fuel gauge — battery voltage, SOC, drain rate ────────────────────
  # Enable with CONFIG_NRF_FUEL_GAUGE=y in prj.conf.
  # Requires CONFIG_CONEXIO_CLOUD_BATTERY_METRICS=y to publish _batt_soc and
  # _batt_drain_hr. LP963450_25C.inc (Conexio Stratus Pro cell model) lives in
  # src/ and is #include'd directly by fuel_gauge.c — no extra include dir needed.
  # No application code required — the SDK calls init and update internally.
  if(CONFIG_NRF_FUEL_GAUGE)
    target_sources(app PRIVATE src/fuel_gauge.c)
  endif()

  # ── CBOR binary encoding — smaller payloads for LTE-M/NB-IoT ─────────────
  # Enable with CONFIG_CONEXIO_CLOUD_CBOR=y in prj.conf.
  # cbor_encoder.h is header-only — no .c file needed.
  # zcbor is pulled in automatically via the ZCBOR Kconfig select.
  if(CONFIG_CONEXIO_CLOUD_CBOR)
    target_compile_definitions(app PRIVATE CONEXIO_CLOUD_CBOR_ENABLED)
  endif()

  # ── Log streaming — Zephyr log backend → cloud telemetry "_log" array ────────
  # Enable with CONFIG_CONEXIO_CLOUD_LOG_STREAM=y in prj.conf.
  if(CONFIG_CONEXIO_CLOUD_LOG_STREAM)
    target_sources(app PRIVATE src/log_stream.c)
  endif()

endif()
