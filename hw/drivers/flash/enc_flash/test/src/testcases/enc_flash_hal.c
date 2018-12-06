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

#include <hal/hal_flash.h>
#include <flash_map/flash_map.h>

#include "enc_flash_test.h"

TEST_CASE(enc_flash_test_hal)
{
    int rc;
    struct flash_area *fa;
    int i;
    int off;
    int blk_sz;
    char writedata[128];
    char readdata[128];

    fa = enc_test_flash_areas;

    for (i = 0; i < ENC_TEST_FLASH_AREA_CNT; i++) {
        rc = hal_flash_erase(fa->fa_id, fa->fa_off, fa->fa_size);
        TEST_ASSERT(rc == 0);
        for (off = 0; off < fa->fa_size; off += blk_sz) {
            blk_sz = fa->fa_size - off;
            if (blk_sz > sizeof(readdata)) {
                blk_sz = sizeof(readdata);
            }
            rc = hal_flash_isempty(fa->fa_id, fa->fa_off + off, readdata,
                                   blk_sz);
            TEST_ASSERT(rc == 1);
        }
        fa++;
    }

    for (i = 0; i < sizeof(writedata); i++) {
        writedata[i] = i;
    }

    fa = enc_test_flash_areas;
    rc = hal_flash_write(fa->fa_id, fa->fa_off, writedata, sizeof(writedata));
    TEST_ASSERT(rc == 0);

    rc = hal_flash_read(fa->fa_id, fa->fa_off, readdata, sizeof(readdata));
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(!memcmp(writedata, readdata, sizeof(writedata)));

    memset(readdata, 0, sizeof(readdata));
    rc = hal_flash_isempty(fa->fa_id, fa->fa_off, readdata, sizeof(readdata));
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(!memcmp(writedata, readdata, sizeof(writedata)));
}
