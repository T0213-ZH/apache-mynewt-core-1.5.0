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

#include "os/mynewt.h"

#if MYNEWT_VAL(LOG_CONSOLE)

#include <cbmem/cbmem.h>
#include <console/console.h>
#include "log/log.h"

static struct log log_console;

struct log *
log_console_get(void)
{
    return &log_console;
}

static void
log_console_print_hdr(const struct log_entry_hdr *hdr)
{
    console_printf("[ts=%lluus, mod=%u level=%u] ",
                   hdr->ue_ts, hdr->ue_module, hdr->ue_level);
}

static int
log_console_append(struct log *log, void *buf, int len)
{
    struct log_entry_hdr *hdr;

    if (!console_is_init()) {
        return (0);
    }

    if (!console_is_midline) {
        hdr = (struct log_entry_hdr *) buf;
        log_console_print_hdr(hdr);
    }

    console_write((char *) buf + LOG_ENTRY_HDR_SIZE, len - LOG_ENTRY_HDR_SIZE);

    return (0);
}

static int
log_console_append_body(struct log *log, const struct log_entry_hdr *hdr,
                        const void *body, int body_len)
{
    if (!console_is_init()) {
        return (0);
    }

    if (!console_is_midline) {
        log_console_print_hdr(hdr);
    }

    console_write(body, body_len);

    return (0);
}

static int
log_console_read(struct log *log, void *dptr, void *buf, uint16_t offset,
        uint16_t len)
{
    /* You don't read console, console read you */
    return (OS_EINVAL);
}

static int
log_console_walk(struct log *log, log_walk_func_t walk_func,
        struct log_offset *log_offset)
{
    /* You don't walk console, console walk you. */
    return (OS_EINVAL);
}

static int
log_console_flush(struct log *log)
{
    /* You don't flush console, console flush you. */
    return (OS_EINVAL);
}

const struct log_handler log_console_handler = {
    .log_type = LOG_TYPE_STREAM,
    .log_read = log_console_read,
    .log_append = log_console_append,
    .log_append_body = log_console_append_body,
    .log_walk = log_console_walk,
    .log_flush = log_console_flush,
};

void
log_console_init(void)
{
    int rc;

    /* Ensure this function only gets called by sysinit. */
    SYSINIT_ASSERT_ACTIVE();

    rc = log_register("console", &log_console, &log_console_handler, NULL,
                      MYNEWT_VAL(LOG_LEVEL));
    SYSINIT_PANIC_ASSERT(rc == 0);
}

#endif
