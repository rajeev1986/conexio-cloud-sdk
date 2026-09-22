#
# sysbuild.cmake — sets the MCUboot signing key path relative to this sample.
# The key lives at sdk_root/keys/ so it's shared across all FOTA samples.
#

# Resolve the SDK root — two levels up from this sample (samples/app → sdk root)
get_filename_component(SDK_ROOT "${APP_DIR}/../.." ABSOLUTE)
set(CONEXIO_SIGNING_KEY "${SDK_ROOT}/keys/conexio-fota-signing.pem")

if(EXISTS "${CONEXIO_SIGNING_KEY}")
  set(SB_CONFIG_BOOT_SIGNATURE_KEY_FILE "${CONEXIO_SIGNING_KEY}"
      CACHE STRING "MCUboot image signing key" FORCE)
  message(STATUS "Conexio FOTA: signing key: ${CONEXIO_SIGNING_KEY}")
else()
  message(WARNING "Conexio FOTA: signing key not found at ${CONEXIO_SIGNING_KEY}. "
                  "Set SB_CONFIG_BOOT_SIGNATURE_KEY_FILE in sysbuild.conf manually.")
endif()
