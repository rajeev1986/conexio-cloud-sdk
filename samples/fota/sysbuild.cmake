#
# sysbuild.cmake — sets the MCUboot signing key path relative to this sample.
#
# SB_CONFIG_BOOT_SIGNATURE_KEY_FILE is a string Kconfig that must be an
# absolute path. It cannot have a relative default in Kconfig because
# string defaults are literals, not expressions.
#
# This file runs at sysbuild CMake time (before Kconfig), resolves the
# signing key path relative to the sample directory, and injects it so
# the developer never needs to edit sysbuild.conf for their machine.
#
# The key is shared across all Conexio SDK FOTA samples — it lives in the
# SDK itself under sdk_root/keys/. Using the same key for all samples means
# devices provisioned with any FOTA sample can accept updates signed by
# any other FOTA sample from this SDK.
#
# ⚠ For production use: generate your own key pair and store it securely.
#   Replace the path below with your key:
#     west ncs-sbom --key path/to/your-signing-key.pem
#   Or set SB_CONFIG_BOOT_SIGNATURE_KEY_FILE in your own sysbuild.conf.

# Resolve the SDK root — two levels up from this sample (samples/fota → sdk root)
get_filename_component(SDK_ROOT "${APP_DIR}/../.." ABSOLUTE)
set(CONEXIO_SIGNING_KEY "${SDK_ROOT}/keys/conexio-fota-signing.pem")

if(EXISTS "${CONEXIO_SIGNING_KEY}")
  set_config_string(mcuboot SB_CONFIG_BOOT_SIGNATURE_KEY_FILE "${CONEXIO_SIGNING_KEY}")
  message(STATUS "Conexio FOTA: signing key: ${CONEXIO_SIGNING_KEY}")
else()
  message(WARNING "Conexio FOTA: signing key not found at ${CONEXIO_SIGNING_KEY}. "
                  "Set SB_CONFIG_BOOT_SIGNATURE_KEY_FILE in sysbuild.conf manually.")
endif()
