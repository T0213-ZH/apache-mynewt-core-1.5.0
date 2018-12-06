/**
 * \file config.h
 *
 * \brief Configuration options (set of defines)
 *
 *  This set of compile-time options may be used to enable
 *  or disable features selectively, and reduce the global
 *  memory footprint.
 *
 *  Copyright (C) 2006-2015, ARM Limited, All Rights Reserved
 *  SPDX-License-Identifier: Apache-2.0
 *
 *  Licensed under the Apache License, Version 2.0 (the "License"); you may
 *  not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  This file is part of mbed TLS (https://tls.mbed.org)
 */

#ifndef MBEDTLS_CONFIG_MYNEWT_H
#define MBEDTLS_CONFIG_MYNEWT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "os/mynewt.h"

#undef MBEDTLS_HAVE_TIME /* we have no time.h */
#undef MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_PRINTF_ALT	console_print
#define MBEDTLS_PLATFORM_EXIT_ALT	assert /* XXX? */
#undef MBEDTLS_FS_IO
#define MBEDTLS_NO_PLATFORM_ENTROPY
#undef MBEDTLS_NET_C

#ifndef TEST
#undef MBEDTLS_SELF_TEST
#endif

#define MBEDTLS_SHA256_SMALLER     /* comes with performance hit */

/**
 * \name SECTION: Module configuration options
 *
 * This section allows for the setting of module specific sizes and
 * configuration options. The default values are already present in the
 * relevant header files and should suffice for the regular use cases.
 *
 * Our advice is to enable options and change their values here
 * only if you have a good reason and know the consequences.
 *
 * Please check the respective header file for documentation on these
 * parameters (to prevent duplicate documentation).
 * \{
 */

/* MPI / BIGNUM options */
//#define MBEDTLS_MPI_WINDOW_SIZE            6 /**< Maximum windows size used. */
//#define MBEDTLS_MPI_MAX_SIZE            1024 /**< Maximum number of bytes for usable MPIs. */

/* CTR_DRBG options */
//#define MBEDTLS_CTR_DRBG_ENTROPY_LEN               48 /**< Amount of entropy used per seed by default (48 with SHA-512, 32 with SHA-256) */
//#define MBEDTLS_CTR_DRBG_RESEED_INTERVAL        10000 /**< Interval before reseed is performed by default */
//#define MBEDTLS_CTR_DRBG_MAX_INPUT                256 /**< Maximum number of additional input bytes */
//#define MBEDTLS_CTR_DRBG_MAX_REQUEST             1024 /**< Maximum number of requested bytes per call */
//#define MBEDTLS_CTR_DRBG_MAX_SEED_INPUT           384 /**< Maximum size of (re)seed buffer */

/* HMAC_DRBG options */
//#define MBEDTLS_HMAC_DRBG_RESEED_INTERVAL   10000 /**< Interval before reseed is performed by default */
//#define MBEDTLS_HMAC_DRBG_MAX_INPUT           256 /**< Maximum number of additional input bytes */
//#define MBEDTLS_HMAC_DRBG_MAX_REQUEST        1024 /**< Maximum number of requested bytes per call */
//#define MBEDTLS_HMAC_DRBG_MAX_SEED_INPUT      384 /**< Maximum size of (re)seed buffer */

/* ECP options */
//#define MBEDTLS_ECP_MAX_BITS             521 /**< Maximum bit size of groups */
//#define MBEDTLS_ECP_WINDOW_SIZE            6 /**< Maximum window size used */
//#define MBEDTLS_ECP_FIXED_POINT_OPTIM      1 /**< Enable fixed-point speed-up */

/* Entropy options */
//#define MBEDTLS_ENTROPY_MAX_SOURCES                20 /**< Maximum number of sources supported */
//#define MBEDTLS_ENTROPY_MAX_GATHER                128 /**< Maximum amount requested from entropy sources */

/* Memory buffer allocator options */
//#define MBEDTLS_MEMORY_ALIGN_MULTIPLE      4 /**< Align on multiples of this value */

/* Platform options */
//#define MBEDTLS_PLATFORM_STD_MEM_HDR   <stdlib.h> /**< Header to include if MBEDTLS_PLATFORM_NO_STD_FUNCTIONS is defined. Don't define if no header is needed. */
//#define MBEDTLS_PLATFORM_STD_CALLOC        calloc /**< Default allocator to use, can be undefined */
//#define MBEDTLS_PLATFORM_STD_FREE            free /**< Default free to use, can be undefined */
//#define MBEDTLS_PLATFORM_STD_EXIT            exit /**< Default exit to use, can be undefined */
//#define MBEDTLS_PLATFORM_STD_FPRINTF      fprintf /**< Default fprintf to use, can be undefined */
//#define MBEDTLS_PLATFORM_STD_PRINTF        printf /**< Default printf to use, can be undefined */
/* Note: your snprintf must correclty zero-terminate the buffer! */
//#define MBEDTLS_PLATFORM_STD_SNPRINTF    snprintf /**< Default snprintf to use, can be undefined */

/* To Use Function Macros MBEDTLS_PLATFORM_C must be enabled */
/* MBEDTLS_PLATFORM_XXX_MACRO and MBEDTLS_PLATFORM_XXX_ALT cannot both be defined */
//#define MBEDTLS_PLATFORM_CALLOC_MACRO        calloc /**< Default allocator macro to use, can be undefined */
//#define MBEDTLS_PLATFORM_FREE_MACRO            free /**< Default free macro to use, can be undefined */
//#define MBEDTLS_PLATFORM_EXIT_MACRO            exit /**< Default exit macro to use, can be undefined */
//#define MBEDTLS_PLATFORM_FPRINTF_MACRO      fprintf /**< Default fprintf macro to use, can be undefined */
//#define MBEDTLS_PLATFORM_PRINTF_MACRO        printf /**< Default printf macro to use, can be undefined */
/* Note: your snprintf must correclty zero-terminate the buffer! */
//#define MBEDTLS_PLATFORM_SNPRINTF_MACRO    snprintf /**< Default snprintf macro to use, can be undefined */

/* SSL Cache options */
//#define MBEDTLS_SSL_CACHE_DEFAULT_TIMEOUT       86400 /**< 1 day  */
//#define MBEDTLS_SSL_CACHE_DEFAULT_MAX_ENTRIES      50 /**< Maximum entries in cache */

/* SSL options */
//#define MBEDTLS_SSL_MAX_CONTENT_LEN             16384 /**< Maxium fragment length in bytes, determines the size of each of the two internal I/O buffers */
//#define MBEDTLS_SSL_DEFAULT_TICKET_LIFETIME     86400 /**< Lifetime of session tickets (if enabled) */
//#define MBEDTLS_PSK_MAX_LEN               32 /**< Max size of TLS pre-shared keys, in bytes (default 256 bits) */
//#define MBEDTLS_SSL_COOKIE_TIMEOUT        60 /**< Default expiration delay of DTLS cookies, in seconds if HAVE_TIME, or in number of cookies issued */

/**
 * Complete list of ciphersuites to use, in order of preference.
 *
 * \warning No dependency checking is done on that field! This option can only
 * be used to restrict the set of available ciphersuites. It is your
 * responsibility to make sure the needed modules are active.
 *
 * Use this to save a few hundred bytes of ROM (default ordering of all
 * available ciphersuites) and a few to a few hundred bytes of RAM.
 *
 * The value below is only an example, not the default.
 */
//#define MBEDTLS_SSL_CIPHERSUITES MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256

/* X509 options */
//#define MBEDTLS_X509_MAX_INTERMEDIATE_CA   8   /**< Maximum number of intermediate CAs in a verification chain. */

/* \} name SECTION: Module configuration options */


/* enable support for configured curves only */
#if MYNEWT_VAL(MBEDTLS_ECP_DP_SECP192R1) == 0
#undef MBEDTLS_ECP_DP_SECP192R1_ENABLED
#endif
#if MYNEWT_VAL(MBEDTLS_ECP_DP_SECP224R1) == 0
#undef MBEDTLS_ECP_DP_SECP224R1_ENABLED
#endif
#if MYNEWT_VAL(MBEDTLS_ECP_DP_SECP256R1) == 0
#undef MBEDTLS_ECP_DP_SECP256R1_ENABLED
#endif
#if MYNEWT_VAL(MBEDTLS_ECP_DP_SECP384R1) == 0
#undef MBEDTLS_ECP_DP_SECP384R1_ENABLED
#endif
#if MYNEWT_VAL(MBEDTLS_ECP_DP_SECP521R1) == 0
#undef MBEDTLS_ECP_DP_SECP521R1_ENABLED
#endif
#if MYNEWT_VAL(MBEDTLS_ECP_DP_SECP192K1) == 0
#undef MBEDTLS_ECP_DP_SECP192K1_ENABLED
#endif
#if MYNEWT_VAL(MBEDTLS_ECP_DP_SECP224K1) == 0
#undef MBEDTLS_ECP_DP_SECP224K1_ENABLED
#endif
#if MYNEWT_VAL(MBEDTLS_ECP_DP_SECP256K1) == 0
#undef MBEDTLS_ECP_DP_SECP256K1_ENABLED
#endif
#if MYNEWT_VAL(MBEDTLS_ECP_DP_BP256R1) == 0
#undef MBEDTLS_ECP_DP_BP256R1_ENABLED
#endif
#if MYNEWT_VAL(MBEDTLS_ECP_DP_BP384R1) == 0
#undef MBEDTLS_ECP_DP_BP384R1_ENABLED
#endif
#if MYNEWT_VAL(MBEDTLS_ECP_DP_BP512R1) == 0
#undef MBEDTLS_ECP_DP_BP512R1_ENABLED
#endif
#if MYNEWT_VAL(MBEDTLS_ECP_DP_CURVE25519) == 0
#undef MBEDTLS_ECP_DP_CURVE25519_ENABLED
#endif

#if MYNEWT_VAL(MBEDTLS_AES_C) == 0
#undef MBEDTLS_AES_C
#endif
#if MYNEWT_VAL(MBEDTLS_ARC4_C) == 0
#undef MBEDTLS_ARC4_C
#endif
#if MYNEWT_VAL(MBEDTLS_BLOWFISH_C) == 0
#undef MBEDTLS_BLOWFISH_C
#endif
#if MYNEWT_VAL(MBEDTLS_CAMELLIA_C) == 0
#undef MBEDTLS_CAMELLIA_C
#endif
#if MYNEWT_VAL(MBEDTLS_DES_C) == 0
#undef MBEDTLS_DES_C
#endif

#if MYNEWT_VAL(MBEDTLS_NIST_KW_C) == 0
#undef MBEDTLS_NIST_KW_C
#endif

#if MYNEWT_VAL(MBEDTLS_CHACHA20_C) == 0
#undef MBEDTLS_CHACHA20_C
#endif
#if MYNEWT_VAL(MBEDTLS_CHACHAPOLY_C) == 0
#undef MBEDTLS_CHACHAPOLY_C
#endif

#if MYNEWT_VAL(MBEDTLS_POLY1305_C) == 0
#undef MBEDTLS_POLY1305_C
#endif

#if MYNEWT_VAL(MBEDTLS_CIPHER_MODE_CBC) == 0
#undef MBEDTLS_CIPHER_MODE_CBC
#endif
#if MYNEWT_VAL(MBEDTLS_CIPHER_MODE_CFB) == 0
#undef MBEDTLS_CIPHER_MODE_CFB
#endif
#if MYNEWT_VAL(MBEDTLS_CIPHER_MODE_CTR) == 0
#undef MBEDTLS_CIPHER_MODE_CTR
#endif
#if MYNEWT_VAL(MBEDTLS_CIPHER_MODE_OFB) == 0
#undef MBEDTLS_CIPHER_MODE_OFB
#endif
#if MYNEWT_VAL(MBEDTLS_CIPHER_MODE_XTS) == 0
#undef MBEDTLS_CIPHER_MODE_XTS
#endif
#if MYNEWT_VAL(MBEDTLS_CCM_C) == 0
#undef MBEDTLS_CCM_C
#endif

#if MYNEWT_VAL(MBEDTLS_CTR_DRBG_C) == 0
#undef MBEDTLS_CTR_DRBG_C
#endif

#if MYNEWT_VAL(MBEDTLS_MD5_C) == 0
#undef MBEDTLS_MD5_C
#endif
#if MYNEWT_VAL(MBEDTLS_SHA1_C) == 0
#undef MBEDTLS_SHA1_C
#endif
#if MYNEWT_VAL(MBEDTLS_SHA512_C) == 0
#undef MBEDTLS_SHA512_C
#endif
#if MYNEWT_VAL(MBEDTLS_RIPEMD160_C) == 0
#undef MBEDTLS_RIPEMD160_C
#endif

#if MYNEWT_VAL(MBEDTLS_MD5_C) == 0 && MYNEWT_VAL(MBEDTLS_SHA1_C) == 0
#undef MBEDTLS_SSL_PROTO_TLS1
#undef MBEDTLS_SSL_PROTO_TLS1_1
#undef MBEDTLS_SSL_CBC_RECORD_SPLITTING
#endif

#if MYNEWT_VAL(MBEDTLS_HKDF_C) == 0
#undef MBEDTLS_HKDF_C
#endif

#if MYNEWT_VAL(MBEDTLS_BASE64_C) == 0
#undef MBEDTLS_BASE64_C
#endif

#if MYNEWT_VAL(MBEDTLS_TIMING_C) == 0
#undef MBEDTLS_TIMING_C
#endif

#if MYNEWT_VAL(MBEDTLS_ENTROPY_C) == 0
#undef MBEDTLS_ENTROPY_C
#endif

#if MYNEWT_VAL(MBEDTLS_PKCS1_V15) == 0
#undef MBEDTLS_PKCS1_V15
#endif
#if MYNEWT_VAL(MBEDTLS_PKCS1_V21) == 0
#undef MBEDTLS_PKCS1_V21
#endif

#if MYNEWT_VAL(MBEDTLS_GENPRIME) == 0
#undef MBEDTLS_GENPRIME
#endif

#if MYNEWT_VAL(MBEDTLS_KEY_EXCHANGE_DHE_RSA_ENABLED) == 0
#undef MBEDTLS_KEY_EXCHANGE_DHE_RSA_ENABLED
#endif
#if MYNEWT_VAL(MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED) == 0
#undef MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#endif
#if MYNEWT_VAL(MBEDTLS_KEY_EXCHANGE_RSA_PSK_ENABLED) == 0
#undef MBEDTLS_KEY_EXCHANGE_RSA_PSK_ENABLED
#endif
#if MYNEWT_VAL(MBEDTLS_KEY_EXCHANGE_RSA_ENABLED) == 0
#undef MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
#endif

#if MYNEWT_VAL(MBEDTLS_SSL_TLS_C) == 0
#undef MBEDTLS_SSL_TLS_C
#undef MBEDTLS_SSL_CLI_C
#undef MBEDTLS_SSL_SRV_C
#undef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#undef MBEDTLS_SSL_DTLS_BADMAC_LIMIT
#endif

#ifdef __cplusplus
}
#endif

#endif /* MBEDTLS_CONFIG_MYNEWT_H */
