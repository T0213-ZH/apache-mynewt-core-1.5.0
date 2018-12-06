/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifndef H_LOG_COMMON_
#define H_LOG_COMMON_

#include "os/mynewt.h"
#include "log_common/ignore.h"

#ifdef __cplusplus
extern "C" {
#endif

struct log;

#define LOG_VERSION_V3  3
#define LOG_VERSION_V2  2
#define LOG_VERSION_V1  1

#define LOG_TYPE_STREAM  (0)
#define LOG_TYPE_MEMORY  (1)
#define LOG_TYPE_STORAGE (2)

#define LOG_LEVEL_DEBUG    (0)
#define LOG_LEVEL_INFO     (1)
#define LOG_LEVEL_WARN     (2)
#define LOG_LEVEL_ERROR    (3)
#define LOG_LEVEL_CRITICAL (4)
/* Up to 7 custom log levels. */
#define LOG_LEVEL_MAX      (UINT8_MAX)

#define LOG_LEVEL_STR(level) \
    (LOG_LEVEL_DEBUG    == level ? "DEBUG"    :\
    (LOG_LEVEL_INFO     == level ? "INFO"     :\
    (LOG_LEVEL_WARN     == level ? "WARN"     :\
    (LOG_LEVEL_ERROR    == level ? "ERROR"    :\
    (LOG_LEVEL_CRITICAL == level ? "CRITICAL" :\
     "UNKNOWN")))))

/* Log module, eventually this can be a part of the filter. */
#define LOG_MODULE_DEFAULT          (0)
#define LOG_MODULE_OS               (1)
#define LOG_MODULE_NEWTMGR          (2)
#define LOG_MODULE_NIMBLE_CTLR      (3)
#define LOG_MODULE_NIMBLE_HOST      (4)
#define LOG_MODULE_NFFS             (5)
#define LOG_MODULE_REBOOT           (6)
#define LOG_MODULE_IOTIVITY         (7)
#define LOG_MODULE_TEST             (8)
#define LOG_MODULE_PERUSER          (64)
#define LOG_MODULE_MAX              (255)

#define LOG_ETYPE_STRING         (0)
#if MYNEWT_VAL(LOG_VERSION) > 2
#define LOG_ETYPE_CBOR           (1)
#define LOG_ETYPE_BINARY         (2)
#endif

/* Logging medium */
#define LOG_STORE_CONSOLE    1
#define LOG_STORE_CBMEM      2
#define LOG_STORE_FCB        3

/* UTC Timestamp for Jan 2016 00:00:00 */
#define UTC01_01_2016    1451606400

#define LOG_NAME_MAX_LEN    (64)

#ifndef MYNEWT_VAL_LOG_LEVEL
#define LOG_SYSLEVEL    ((uint8_t)0xff)
#else
#define LOG_SYSLEVEL    ((uint8_t)MYNEWT_VAL_LOG_LEVEL)
#endif

/* Newtmgr Log opcodes */
#define LOGS_NMGR_OP_READ         	(0)
#define LOGS_NMGR_OP_CLEAR        	(1)
#define LOGS_NMGR_OP_APPEND       	(2)
#define LOGS_NMGR_OP_MODULE_LIST  	(3)
#define LOGS_NMGR_OP_LEVEL_LIST   	(4)
#define LOGS_NMGR_OP_LOGS_LIST    	(5)
#define LOGS_NMGR_OP_SET_WATERMARK	(6)

#define LOG_PRINTF_MAX_ENTRY_LEN (128)

/* Global log info */
struct log_info {
    uint32_t li_next_index;
    uint8_t li_version;
};

extern struct log_info g_log_info;

/** @typedef log_append_cb
 * @brief Callback that is executed each time the corresponding log is appended
 * to.
 *
 * @param log                   The log that was just appended to.
 * @param idx                   The index of newly appended log entry.
 */
typedef void log_append_cb(struct log *log, uint32_t idx);

#ifdef __cplusplus
}
#endif

#endif
