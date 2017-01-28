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
 * @brief UNFS file read-modified-write (i.e. page-unaligned write) test.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

#include "unfs.h"
#include "unfs_log.h"


/// Usage
static const char*  usage =
"\nUsage: %s [OPTION]... DEVICE_NAME\n\
          -v              turn on verbose\n\
          -n NSID         NVMe namespace id (default 1)\n\
          -t THREADCOUNT  number of threads (default 64)\n\
          DEVICE_NAME     device name\n";

static unfs_fs_t    fs;                     ///< filesystem handle
static int          verbose = 0;            ///< verbose flag
static int          thread_count = 64;      ///< thread count
static sem_t        sm_ready;               ///< thread ready semaphore
static sem_t        sm_run;                 ///< thread run test semaphore

/// Test entry structure
typedef struct {
    u64         filesize;       ///< file size
    u64         offset;         ///< read/write offset
    u64         len;            ///< read/write length
} test_entry_t;

/// Test table
test_entry_t test_table[] = {
    //  filesize        offset          length
    {   1,              0,              1           },
    {   4000,           0,              4000        },
    {   4000,           0,              3999        },
    {   4000,           2001,           1999        },
    {   4000,           3000,           900         },
    {   8000,           0,              8000        },
    {   8000,           7999,           1           },
    {   8000,           2,              4094        },
    {   8000,           4096,           1           },
    {   8000,           4097,           3003        },
    {  12000,           0,              7000        },
    {  12000,           2,              4094        },
    {  12000,           1,              1           },
    {  12000,           4096,           1           },
    {  12000,           6000,           6000        },
    {  12000,           0,              12000       },
    {  16000,           0,              7000        },
    {  16000,           15999,          1           },
    {  16000,           1,              1           },
    {  16000,           8192,           7000        },
    {  16000,           8000,           8000        },
    {  16000,           0,              12000       },
    {  32768,           0,              32768       },
    {  32768,           16382,          16384       },
    {  32768,           512,            16381       },
    {  32768,           0,              12          },
    {  65501,           0,              65501       },
    {  65501,           0,              60000       },
    {  65501,           123,            65378       },
    {  65501,           1,              23456       },
    {  65501,           65000,          500         },
    {  65501,           5001,           40001       },
    {  262000,          0,              262000      },
    {  262000,          0,              261001      },
    {  262000,          2000,           260000      },
    {  262000,          1111,           1111        },
    {  262000,          22222,          222222      },
    {  262000,          233333,         1           },
    {  262000,          260000,         1000        },
    {  0,               0,              0           }
};

/// Print if verbose flag is set
#define VERBOSE(fmt, arg...) if (verbose) printf(fmt, ##arg)

/// Print fatal error message and exit
#define FATAL(fmt, arg...) do { ERROR(fmt, ##arg); exit(1); } while (0)


/**
 * Test file read/write of a given entry with offset, length, pattern, and size. 
 */
void test_rmw(unfs_fd_t fd, test_entry_t* t, int pat)
{
    char name[64];
    unfs_file_name(fd, name, sizeof(name));
    VERBOSE("# %s off=%-6lu len=%-6lu size=%-6ld pat=0x%02x\n",
            name, t->offset, t->len, t->filesize, pat);
    uint8_t* wbuf = malloc(t->filesize);
    uint8_t* rbuf = malloc(t->filesize);

    // resize file
    if (unfs_file_resize(fd, t->filesize, 0))
        FATAL("Resize failed");
        
    // fill file with 0xff
    memset(wbuf, 0xff, t->filesize);
    unfs_file_write(fd, wbuf, 0, t->filesize);

    // read and verify the whole file
    unfs_file_read(fd, rbuf, 0, t->filesize);
    u64 i;
    for (i = 0; i < t->filesize; i++) {
        if (rbuf[i] != 0xff)
            FATAL("Data mismatch off=%lu w=0xff r=0x%02x", i, rbuf[i]);
    }

    // write test data to file
    memset(wbuf + t->offset, pat, t->len);
    unfs_file_write(fd, wbuf + t->offset, t->offset, t->len);

    // read and verify the whole file
    unfs_file_read(fd, rbuf, 0, t->filesize);
    for (i = 0; i < t->filesize; i++) {
        if (rbuf[i] != wbuf[i])
            FATAL("Data mismatch off=%lu w=0x%02x r=0x%02x", i, wbuf[i], rbuf[i]);
    }

    free(wbuf);
    free(rbuf);
}

/**
 * Thread to perform read-modified-write test.
 */
static void* test_thread(void* arg)
{
    long tid = (long)arg;
    int pat = tid;

    // send ready and wait for signal to run test
    sem_post(&sm_ready);
    sem_wait(&sm_run);

    // Create the test file
    char filename[64];
    sprintf(filename, "/rmw%ld", tid);
    printf("Create and test %s\n", filename);
    unfs_fd_t fd = unfs_file_open(fs, filename, UNFS_OPEN_CREATE);
    if (fd.error)
        FATAL("create %s (%s)", filename, strerror(fd.error));

    // Run table entry tests forward
    test_entry_t* t = test_table;
    while (t->filesize > 0) {
        pat++;
        test_rmw(fd, t, pat);
        t++;
    }
    unfs_file_close(fd);

    // Run table entry tests backward
    fd = unfs_file_open(fs, filename, 0);
    if (fd.error)
        FATAL("open %s (%s)", filename, strerror(fd.error));
    do {
        pat++;
        t--;
        test_rmw(fd, t, pat);
    } while (t != test_table);
    if (unfs_file_resize(fd, tid, 0))
        FATAL("Resize failed");
    unfs_file_close(fd);

    return 0;
}

/**
 * Main program.
 */
int main(int argc, char** argv)
{
    const char* prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];
    int opt, i;

    while ((opt = getopt(argc, argv, "n:t:v")) != -1) {
        switch (opt) {
        case 'v':
            verbose = 1;
            break;
        case 'n':
            setenv("UNFS_NSID", optarg, 1);
            break;
        case 't':
            thread_count = atoi(optarg);
            if (thread_count <= 0)
                FATAL("Thread count must be > 0");
            break;
        default:
            fprintf(stderr, usage, prog);
        }
    }

    const char* device = getenv("UNFS_DEVICE");
    if (optind < argc) {
        device = argv[optind];
    } else if (!device) {
        fprintf(stderr, usage, prog);
        exit(1);
    }

    LOG_OPEN();
    printf("UNFS READ-MODIFIED-WRITE TEST BEGIN\n");

    // Format new UNFS filesystem
    printf("UNFS format device %s\n", device);
    if (unfs_format(device, prog, 0))
        FATAL("UNFS format failed");

    // Open to access UNFS filesystem
    printf("UNFS open device %s\n", device);
    setenv("UNFS_IOMEMPC", "4", 1);     // test with small number of IO pages
    fs = unfs_open(device);
    if (!fs)
        FATAL("UNFS open failed");

    // Spawn test threads
    time_t tstart = time(0);
    sem_init(&sm_ready, 0, 0);
    sem_init(&sm_run, 0, 0);
    pthread_t* pts = calloc(thread_count, sizeof(pthread_t));
    for (i = 0; i < thread_count; i++) {
        pthread_create(&pts[i], 0, test_thread, (void*)(long)(i + 1));
        sem_wait(&sm_ready);
    }
    for (i = 0; i < thread_count; i++)
        sem_post(&sm_run);
    for (i = 0; i < thread_count; i++)
        pthread_join(pts[i], 0);
    free(pts);
    sem_destroy(&sm_ready);
    sem_destroy(&sm_run);
    unfs_close(fs);

    // Reopen filesystem and verify files
    printf("UNFS reopen device %s\n", device);
    fs = unfs_open(device);
    if (!fs)
        FATAL("UNFS open failed");
    for (i = 1; i <= thread_count; i++) {
        char filename[64];
        sprintf(filename, "/rmw%d", i);
        printf("Verify %s size %d\n", filename, i);
        u64 size = 0;
        if (!unfs_exist(fs, filename, 0, &size))
            FATAL("%s does not exist", filename);
        if (size != i)
            FATAL("%s size %ld expect %d", filename, size, i);
    }

    // Verify filesystem info
    unfs_header_t hdr;
    printf("~~~~~~~~~~~~~~~~~~~~~~~~~~\n");
    if (unfs_stat(fs, &hdr, 1))
        FATAL("UNFS status error");
    printf("~~~~~~~~~~~~~~~~~~~~~~~~~~\n");
    unfs_close(fs);
    u64 fdcount = thread_count + 1;
    if (hdr.fdcount != fdcount)
        FATAL("FD count %#lx expect %#lx", hdr.fdcount, fdcount);
    if (hdr.dircount != 1)
        FATAL("Dir count %ld expect 1", hdr.dircount);
    u64 fdpage = hdr.pagecount - (fdcount + 1) * UNFS_FILEPC;
    if (hdr.fdpage != fdpage)
        FATAL("FD page %#lx expect %#lx", hdr.fdcount, fdpage);
    u64 pagefree = hdr.pagecount - (fdcount * UNFS_FILEPC + thread_count);
    if (hdr.pagefree != pagefree)
        FATAL("Free pages %#lx expect %#lx", hdr.pagefree, pagefree);

    if (unfs_check(device))
        return 1;

    printf("UNFS READ-MODIFIED-WRITE TEST COMPLETE (%ld secs)\n", time(0) - tstart);
    LOG_CLOSE();

    return 0;
}
