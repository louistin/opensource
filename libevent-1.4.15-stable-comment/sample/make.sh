#!/bin/bash

# 确保运行时正确链接动态库
gcc -I/opt/libevent/include -o event-test event-test.c -L/opt/libevent/lib /opt/libevent/lib/libevent-1.4.so.2 -Wl,-rpath=/opt/libevent/lib
gcc -o event-test-write event-test-write.c
