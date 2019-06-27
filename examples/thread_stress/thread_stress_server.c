// Copyright (C) 2018, Jaguar Land Rover
// This program is licensed under the terms and conditions of the
// Mozilla Public License, version 2.0.  The full text of the
// Mozilla Public License is at https://www.mozilla.org/MPL/2.0/
//
// Author: Magnus Feuer (mfeuer1@jaguarlandrover.com)
//
// Running example code from README.md in https://github.com/PDXOSTC/dstc
//

#include <stdio.h>
#include <stdlib.h>
#include "dstc.h"
#include <unistd.h>
#include <pthread.h>
#include <errno.h>

// Generate deserializer for multicast packets sent by the client
// The deserializer decodes the incoming data and calls the
// set_value() function in this file.
//
DSTC_SERVER(set_value1, int,)
DSTC_SERVER(set_value2, int,)
DSTC_SERVER(set_value3, int,)
DSTC_SERVER(set_value4, int,)

int exit_flag[4] = { 0 };


void _set_value(int thr, int value, int *last_value, usec_timestamp_t* start_ts)
{
    if (!*start_ts)
        *start_ts = rmc_usec_monotonic_timestamp();

    if (value == -1) {
        usec_timestamp_t stop_ts = rmc_usec_monotonic_timestamp();
        (*last_value)++;
        printf("Thread[%d] Processed %d calls in %.2f sec -> %.2f calls/sec\n",
               thr,
               *last_value,
               (stop_ts - *start_ts) / 1000000.0,
               *last_value / ((stop_ts - *start_ts) / 1000000.0));
        dstc_process_pending_events();
        exit_flag[thr] = 1;
        return;
    }

    if (value % 100000 == 0)
        printf("Thread[%d] Value: %d\n", thr, value);

    // Check that we got the expected value.
    if (*last_value != -1 && value != *last_value + 1 ) {
        printf("Thread[%d] Integrity failure!  Want value %d Got value %d\n",
               thr, *last_value + 1 , value);
        exit(255);
    }
    *last_value = value;

}
//
// Receive a value and check its integrity
// Invoked by deserilisation code generated by DSTC_SERVER() above.
// Please note that the arguments must match between the function below
// and the macro above.
//
void set_value1(int value)
{
    static int last_value = -1;
    static usec_timestamp_t start_ts = 0;

    _set_value(0, value, &last_value, &start_ts);
}

void set_value2(int value)
{
    static int last_value = -1;
    static usec_timestamp_t start_ts = 0;

    _set_value(1,value, &last_value, &start_ts);
}

void set_value3(int value)
{
    static int last_value = -1;
    static usec_timestamp_t start_ts = 0;

    _set_value(2, value, &last_value, &start_ts);
}

void set_value4(int value)
{
    static int last_value = -1;
    static usec_timestamp_t start_ts = 0;

    _set_value(3, value, &last_value, &start_ts);
}

void *t_exec(void* arg)
{
    int thr = (intptr_t) arg;

    while(!exit_flag[thr])
        dstc_process_events(-1);

    printf("Thread %d is exiting\n", thr);
    return 0;
}

int main(int argc, char* argv[])
{
    pthread_t t1;
    pthread_t t2;
    pthread_t t3;
    pthread_t t4;
    int res = 0;
    dstc_setup();
    res = pthread_create(&t1, 0, t_exec, (void*) 0);
    if (res) {
        perror("thr1");
        exit(255);
    }

    res = pthread_create(&t2, 0, t_exec, (void*) 1);
    if (res) {
        perror("thr2");
        exit(255);
    }
    res = pthread_create(&t3, 0, t_exec, (void*) 2);
    if (res) {
        perror("thr3");
        exit(255);
    }
    res = pthread_create(&t4, 0, t_exec, (void*) 3);

    if (res) {
        perror("thr4");
        exit(255);
    }

    pthread_join(t1, 0);
    puts("Joined thread 1");
    pthread_join(t2, 0);
    puts("Joined thread 2");
    pthread_join(t3, 0);
    puts("Joined thread 3");
    pthread_join(t4, 0);
    puts("Joined thread 4");

    exit(0);
}
