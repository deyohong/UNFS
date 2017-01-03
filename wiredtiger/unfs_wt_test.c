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
 * @brief WiredTiger filesystem quick test.
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <error.h>

#include "unfs.h"
#include "unfs_wt.h"


/// Usage
static const char* usage =
"\nUsage: %s [OPTION]... DEVICE_NAME\n\
          -n NSID        NVMe namespace id (default N=1)\n\
          -q QCOUNT      NVMe queue count (default 16)\n\
          -d QDEPTH      NVMe queue depth (default 32)\n\
          -h HOMEDIR     home directory\n\
          DEVICE_NAME    device name\n";


/**
 * Main program.
 */
int main(int argc, char** argv)
{
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    const char *key;
    char kbuf[64];
    int i, opt;
    int ret = 0;
    const char* home = "WT_HOME";

    const char* prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];

    while ((opt = getopt(argc, argv, "n:q:d:p:h:")) != -1) {
        switch (opt) {
        case 'n':
            setenv("UNFS_NSID", optarg, 1);
            break;
        case 'q':
            setenv("UNFS_QCOUNT", optarg, 1);
            break;
        case 'd':
            setenv("UNFS_QDEPTH", optarg, 1);
            break;
        case 'h':
            home = optarg;
            break;
        default:
            error(1, 0, usage, prog);
        }
    }

    const char* device = getenv("UNFS_DEVICE");
    if (optind < argc) {
        device = argv[optind];
        setenv("UNFS_DEVICE", device, 1);
    } else if (!device) {
        error(1, 0, usage, prog);
    }

    printf("WIREDTIGER UNFS EXAMPLE FILESYSTEM TEST BEGIN\n");

    // create filesystem
    printf("UNFS format device %s\n", device);
    ret = unfs_format(device, "WiredTiger", 0);
    if (ret) return ret;

    //=======================================================================
    // The following code section is taken from the WiredTiger
    // examples/c/ex_filesystem.c with changes to the config string
    // in order to verify basic WiredTiger on UNFS functionalities.
    //=======================================================================
    char* config = unfs_wiredtiger_config(home, "create,log=(enabled=true)");
    if ((ret = wiredtiger_open(home, NULL, config, &conn)) != 0) {
        fprintf(stderr, "wiredtiger_open %s: %s\n", home, wiredtiger_strerror(ret));
        free(config);
        return EXIT_FAILURE;
    }

    if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
        fprintf(stderr, "WT_CONNECTION.open_session: %s\n",
                wiredtiger_strerror(ret));
        free(config);
        return (EXIT_FAILURE);
    }
    const char* uri = "table:fs";
    if ((ret = session->create(
            session, uri, "key_format=S,value_format=S")) != 0) {
        fprintf(stderr, "WT_SESSION.create: %s: %s\n",
                uri, wiredtiger_strerror(ret));
        free(config);
        return (EXIT_FAILURE);
    }
    if ((ret = session->open_cursor(
            session, uri, NULL, NULL, &cursor)) != 0) {
        fprintf(stderr, "WT_SESSION.open_cursor: %s: %s\n",
                uri, wiredtiger_strerror(ret));
        free(config);
        return (EXIT_FAILURE);
    }
    for (i = 0; i < 1000; ++i) {
        (void) snprintf(kbuf, sizeof (kbuf), "%010d KEY -----", i);
        cursor->set_key(cursor, kbuf);
        cursor->set_value(cursor, "--- VALUE ---");
        if ((ret = cursor->insert(cursor)) != 0) {
            fprintf(stderr, "WT_CURSOR.insert: %s: %s\n",
                    kbuf, wiredtiger_strerror(ret));
            free(config);
            return (EXIT_FAILURE);
        }
    }
    if ((ret = cursor->close(cursor)) != 0) {
        fprintf(stderr, "WT_CURSOR.close: %s\n",
                wiredtiger_strerror(ret));
        free(config);
        return (EXIT_FAILURE);
    }
    if ((ret = session->open_cursor(
            session, uri, NULL, NULL, &cursor)) != 0) {
        fprintf(stderr, "WT_SESSION.open_cursor: %s: %s\n",
                uri, wiredtiger_strerror(ret));
        free(config);
        return (EXIT_FAILURE);
    }
    for (i = 0; i < 1000; ++i) {
        if ((ret = cursor->next(cursor)) != 0) {
            fprintf(stderr, "WT_CURSOR.insert: %s: %s\n",
                    kbuf, wiredtiger_strerror(ret));
            free(config);
            return (EXIT_FAILURE);
        }
        (void) snprintf(kbuf, sizeof (kbuf), "%010d KEY -----", i);
        if ((ret = cursor->get_key(cursor, &key)) != 0) {
            fprintf(stderr, "WT_CURSOR.get_key: %s\n",
                    wiredtiger_strerror(ret));
            free(config);
            return (EXIT_FAILURE);
        }
        if (strcmp(kbuf, key) != 0) {
            fprintf(stderr, "Key mismatch: %s, %s\n", kbuf, key);
            free(config);
            return (EXIT_FAILURE);
        }
    }
    if ((ret = cursor->next(cursor)) != WT_NOTFOUND) {
        fprintf(stderr,
                "WT_CURSOR.insert: expected WT_NOTFOUND, got %s\n",
                wiredtiger_strerror(ret));
        free(config);
        return (EXIT_FAILURE);
    }

    if ((ret = conn->close(conn, NULL)) != 0) {
        fprintf(stderr, "Error closing connection to %s: %s\n",
                home == NULL ? "." : home, wiredtiger_strerror(ret));
        free(config);
        return (EXIT_FAILURE);
    }

    printf("WIREDTIGER UNFS EXAMPLE FILESYSTEM TEST COMPLETE\n");
    free(config);
    return 0;
}

