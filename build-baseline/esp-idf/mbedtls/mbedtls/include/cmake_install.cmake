# Install script for directory: /root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "TRUE")
endif()

# Set default install directory permissions.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/root/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20251107/riscv32-esp-elf/bin/riscv32-esp-elf-objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/mbedtls" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ FILES
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/aes.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/aria.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/asn1.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/asn1write.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/base64.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/bignum.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/block_cipher.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/build_info.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/camellia.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/ccm.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/chacha20.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/chachapoly.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/check_config.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/cipher.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/cmac.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/compat-2.x.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/config_adjust_legacy_crypto.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/config_adjust_legacy_from_psa.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/config_adjust_psa_from_legacy.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/config_adjust_psa_superset_legacy.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/config_adjust_ssl.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/config_adjust_x509.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/config_psa.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/constant_time.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/ctr_drbg.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/debug.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/des.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/dhm.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/ecdh.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/ecdsa.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/ecjpake.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/ecp.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/entropy.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/error.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/gcm.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/hkdf.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/hmac_drbg.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/lms.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/mbedtls_config.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/md.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/md5.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/memory_buffer_alloc.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/net_sockets.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/nist_kw.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/oid.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/pem.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/pk.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/pkcs12.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/pkcs5.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/pkcs7.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/platform.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/platform_time.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/platform_util.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/poly1305.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/private_access.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/psa_util.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/ripemd160.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/rsa.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/sha1.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/sha256.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/sha3.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/sha512.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/ssl.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/ssl_cache.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/ssl_ciphersuites.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/ssl_cookie.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/ssl_ticket.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/threading.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/timing.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/version.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/x509.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/x509_crl.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/x509_crt.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/mbedtls/x509_csr.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/psa" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ FILES
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/psa/build_info.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/psa/crypto.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/psa/crypto_adjust_auto_enabled.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/psa/crypto_adjust_config_dependencies.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/psa/crypto_adjust_config_key_pair_types.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/psa/crypto_adjust_config_synonyms.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/psa/crypto_builtin_composites.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/psa/crypto_builtin_key_derivation.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/psa/crypto_builtin_primitives.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/psa/crypto_compat.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/psa/crypto_config.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/psa/crypto_driver_common.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/psa/crypto_driver_contexts_composites.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/psa/crypto_driver_contexts_key_derivation.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/psa/crypto_driver_contexts_primitives.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/psa/crypto_extra.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/psa/crypto_legacy.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/psa/crypto_platform.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/psa/crypto_se_driver.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/psa/crypto_sizes.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/psa/crypto_struct.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/psa/crypto_types.h"
    "/root/esp/esp-idf-v5.5.3/components/mbedtls/mbedtls/include/psa/crypto_values.h"
    )
endif()

