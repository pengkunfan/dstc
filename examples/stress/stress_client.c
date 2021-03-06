// Copyright (C) 2018, Jaguar Land Rover
// This program is licensed under the terms and conditions of the
// Mozilla Public License, version 2.0.  The full text of the
// Mozilla Public License is at https://www.mozilla.org/MPL/2.0/
//
// Author: Magnus Feuer (mfeuer1@jaguarlandrover.com)
//
// Running example code from README.md in https://github.com/PDXOSTC/dstc
//

#include "dstc.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include "rmc_log.h"

// Generate serializer functionality and the callable client function
// dstc_set_value(), which will invoke the remote server process'
// set_value() function.
//
DSTC_CLIENT(set_value, int,)

int main(int argc, char* argv[])
{
    int val = 0;
    // Wait for function to become available on one or more servers.
    while(!dstc_remote_function_available(dstc_set_value))
        dstc_process_events(-1);


    // Move into buffered mode to transmit 63K UDP packets.
    dstc_buffer_client_calls();
    //
    // Pump as many calls as we can through the server.
    // If we choke on EBUSY, process events until we have cleared
    // the output queue enough to continue.
    //
    while(val < 10000000) {
        // Pump out calls until we get EBUSY back.
        //
        // At that point, process events to actually send them
        // over the wire until the dstc_set_value() call succeeeds.
        while (dstc_set_value(val) == EBUSY) {
            dstc_process_events(1);
            continue;
        }

        if (val % 100000 == 0)
            printf("Client value: %d\n", val);

        ++val;
    }

    // Unbuffer call sequences to ensure that we get
    // all final calls go out.
    dstc_unbuffer_client_calls();
    puts("Client telling server to exit");
    int ret = 0;
    while ((ret = dstc_set_value(-1)) == EBUSY) {
        dstc_process_events(0);
        continue;
    }

    puts("Processing events telling server to exit");
    // Process events until there are no more.
    msec_timestamp_t ts = dstc_msec_monotonic_timestamp();
    msec_timestamp_t timeout = ts + 2000;
    while(ts < timeout) {
        dstc_process_events(timeout - ts);
        ts = dstc_msec_monotonic_timestamp();
    }

    puts("Client exiting");
    exit(0);
}
