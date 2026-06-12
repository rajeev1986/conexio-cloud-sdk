cmake_minimum_required(VERSION 3.20.0)

if(CONFIG_CONEXIO_CLOUD)

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

endif()
