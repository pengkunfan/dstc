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

// Define both a client and a server that can be called from the
// same function
//
DSTC_CLIENT(loopback, char, [32], int,)
DSTC_SERVER(loopback, char, [32], int,)

//
// Print out name and age.
// Invoked by deserilisation code generated by DSTC_SERVER() above.
// Please note that the arguments must match between the function below
// and the macro above.
//
void loopback(char name[32], int age)
{
    printf("Name: %s\n", name);
    printf("Age:  %d\n", age);
    exit(0);
}

int main(int argc, char* argv[])
{
    char name[32] = {0};
    // Wait for function to become available on one or more servers.

    while(!dstc_remote_function_available(dstc_loopback))
        dstc_process_events(-1);

    strcpy(name, "Bob Smith");

    // Send out a loopback call to ourselves.
    dstc_loopback(name, 25);

    // Process events for another 100 msec to ensure that the call gets out.
    // This loop will also pick up the call and execute it.
    while(1)
        dstc_process_events(-1);
}
