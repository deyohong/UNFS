/**
 * Copyright (c) 2016-2017, Micron Technology, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 *   3. Neither the name of the copyright holder nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 * @brief Create UNFS format on a given device.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <err.h>

#include "unfs.h"


/// Usage
static const char*  usage =
"\nUsage: %s [OPTION]... DEVICE_NAME\n\
          -n NSID       NVMe namespace id (default 1)\n\
          -l LABEL      label\n\
          -q            quiet do not print out status\n\
          DEVICE_NAME   device name\n";


/**
 * Main program.
 */
int main(int argc, char** argv)
{
    const char* prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];
    int quiet = 0;

    const char* label = "User Space Nameless Filesystem";
    int opt;

    while ((opt = getopt(argc, argv, "n:l:q")) != -1) {
        switch (opt) {
        case 'n':
            setenv("UNFS_NSID", optarg, 1);
            break;
        case 'l':
            label = optarg;
            break;
        case 'q':
            quiet = 1;
            break;
        default:
            errx(1, usage, prog);
        }
    }

    const char* device = getenv("UNFS_DEVICE");
    if (optind < argc) {
        device = argv[optind];
    } else if (!device) {
        errx(1, usage, prog);
    }

    if (!quiet)
        printf("UNFS format device %s label \"%s\"\n", device, label);

    return unfs_format(device, label, !quiet);
}

