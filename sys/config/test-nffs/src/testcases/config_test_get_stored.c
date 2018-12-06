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
#include "conf_test_nffs.h"

TEST_CASE(config_test_get_stored_file)
{
    int rc;
    struct conf_file cf;
    char stored_val[32];

    config_wipe_srcs();
    rc = fs_mkdir("/config");
    TEST_ASSERT(rc == 0 || rc == FS_EEXIST);

    cf.cf_name = "/config/blah";
    rc = conf_file_src(&cf);
    TEST_ASSERT(rc == 0);
    rc = conf_file_dst(&cf);
    TEST_ASSERT(rc == 0);

    test_export_block = 0;
    val8 = 33;
    rc = conf_save();
    TEST_ASSERT(rc == 0);

    /*
     * Nonexistent key
     */
    rc = conf_get_stored_value("random/name", stored_val, sizeof(stored_val));
    TEST_ASSERT(rc == OS_ENOENT);

    rc = conf_get_stored_value("myfoo/mybar", stored_val, sizeof(stored_val));
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(atoi(stored_val) == 33);

    rc = conf_save_one("myfoo/mybar", "42");
    TEST_ASSERT(rc == 0);

    rc = conf_get_stored_value("myfoo/mybar", stored_val, sizeof(stored_val));
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(atoi(stored_val) == 42);

    val8 = 31;
    rc = conf_save();
    TEST_ASSERT(rc == 0);

    rc = conf_get_stored_value("myfoo/mybar", stored_val, sizeof(stored_val));
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(atoi(stored_val) == 31);

    /*
     * Too small of a buffer
     */
    rc = conf_get_stored_value("myfoo/mybar", stored_val, 1);
    TEST_ASSERT(rc == OS_EINVAL);

    test_export_block = 1;
}
