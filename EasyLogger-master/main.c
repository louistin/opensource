/*
 * This file is part of the EasyLogger Library.
 *
 * Copyright (c) 2015-2017, Armink, <armink.ztl@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * 'Software'), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED 'AS IS', WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Function: linux demo.
 * Created on: 2015-07-30
 */

#define LOG_TAG    "basic"

#include "elog.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main() {
    uint8_t buf[256]= {0};
    int i = 0;

    for (i = 0; i < sizeof(buf); i++)
    {
        buf[i] = i;
    }

    easylogger_init();

    while(true) {
        /* test log output for all level */
        log_a("Hello EasyLogger!");
        log_e("Hello EasyLogger!");
        log_w("Hello EasyLogger!");
        log_i("Hello EasyLogger!");
        log_d("Hello EasyLogger!");
        log_v("Hello EasyLogger!");
        //elog_raw("Hello EasyLogger!");
        elog_hexdump("test", 16, buf, sizeof(buf));
        sleep(5);
    }
}
