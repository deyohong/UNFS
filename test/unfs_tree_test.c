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
 * @brief UNFS complex tree test.
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
static const char* usage =
"\nUsage: %s [OPTION]... DEVICE_NAME\n\
          -v              turn on verbose\n\
          -n NSID         NVMe namespace id (default 1)\n\
          -t THREADCOUNT  number of threads (default 32)\n\
          -d DEPTH        tree depth per thread (default 8)\n\
          -f FILECOUNT    number of files per directory (default 4)\n\
          DEVICE_NAME     device name\n";

static unfs_fs_t    fs;                     ///< filesystem handle
static int          verbose = 0;            ///< verbose flag
static int          thread_count = 32;      ///< number of threads
static int          tree_depth = 8;         ///< directory tree depth
static int          file_count = 4;         ///< number of files per directory
static int          small_file = 0;         ///< small file for quick test
static sem_t        sm_ready;               ///< thread ready semaphore
static sem_t        sm_run;                 ///< thread run test semaphore

/// Print if verbose flag is set
#define VERBOSE(fmt, arg...) if (verbose) printf(fmt, ##arg)

/// Print fatal error message and exit
#define FATAL(fmt, arg...) do { ERROR(fmt, ##arg); exit(1); } while (0)


/**
 * Mark a file by writing its node info at the end.
 */
static void mark_file(const char* filename)
{
    unfs_fd_t fd = unfs_file_open(fs, filename, 0);
    if (fd.error)
        FATAL("Open %s (%s)", filename, strerror(fd.error));

    // adjust file size if necessary
    unfs_node_io_t niop;
    u64 size;
    u32 dsc;
    unfs_ds_t* dslp;
    unfs_file_stat(fd, &size, &dsc, &dslp);
    if (size < sizeof(niop)) {
        size = sizeof(niop);
        unfs_file_resize(fd, size, 0);
        unfs_file_stat(fd, &size, &dsc, &dslp);
    }
    VERBOSE("# mark file %s %ld %d\n", filename, size, dsc);

    // setup unique 8K pattern
    memset(&niop, (int)size, sizeof(niop));
    niop.node.size = size;
    niop.node.dscount = dsc;
    memcpy(niop.node.ds, dslp, dsc * sizeof(unfs_ds_t));
    strcpy(niop.name, filename);
    free(dslp);

    // fill file with pattern
    u64 pos = 0;
    while (pos < size) {
        u64 n = size - pos;
        if (n > sizeof(niop)) n = sizeof(niop);
        unfs_file_write(fd, &niop, pos, n);
        pos += n;
    }

    unfs_file_close(fd);
}

/**
 * Check a file mark.
 */
static void check_file(const char* filename)
{
    VERBOSE("# check file %s\n", filename);
    unfs_fd_t fd = unfs_file_open(fs, filename, 0);
    if (fd.error)
        FATAL("Open %s (%s)", filename, strerror(fd.error));

    // check file size
    unfs_node_io_t wniop, rniop;
    u64 size;
    u32 dsc;
    unfs_ds_t* dslp;
    unfs_file_stat(fd, &size, &dsc, &dslp);
    if (size < sizeof(wniop))
        FATAL("%s has invalid size %ld", filename, size);

    // setup unique 8K pattern
    memset(&wniop, (int)size, sizeof(wniop));
    wniop.node.size = size;
    wniop.node.dscount = dsc;
    memcpy(wniop.node.ds, dslp, dsc * sizeof(unfs_ds_t));
    strcpy(wniop.name, filename);
    free(dslp);

    // fill file with pattern
    u64 pos = 0;
    while (pos < size) {
        u64 n = size - pos;
        if (n > sizeof(rniop))
            n = sizeof(rniop);
        unfs_file_read(fd, &rniop, pos, n);
        if (memcmp(&wniop, &rniop, sizeof(rniop)))
            FATAL("%s has invalid data", filename);
        pos += n;
    }

    unfs_file_close(fd);
}

/**
 * Check the test tree created.
 */
static void check_tree(int tid)
{
    u64 size;
    int d, f, isdir;
    char name[UNFS_MAXPATH+1];
    sprintf(name, "/tree%d", tid);
    size_t dlen = strlen(name);

    printf("Verify %s\n", name);
    for (d = 1; d <= tree_depth; d++) {
        snprintf(name + dlen, sizeof (name), "/dir%d", d);
        u64 exp = file_count + 1;
        if (d < tree_depth)
            exp++;
        VERBOSE("# check %s size %ld\n", name, exp);
        dlen = strlen(name);
        isdir = 0;
        size = 0;
        if (!unfs_exist(fs, name, &isdir, &size))
            FATAL("%s does not exist", name);
        if (!isdir || size != exp)
            FATAL("%s size %ld expect %ld", name, size, exp);
        for (f = 1; f <= file_count; f++) {
            snprintf(name + dlen, sizeof (name), "/file%d", f);
            if (f == 1)
                strcat(name, "x");
            check_file(name);
        }
    }
}

/**
 * Create a complex tree of given depth and number of files.
 */
static void test_tree(int tid)
{
    char name[UNFS_MAXPATH];
    char tmpname[UNFS_MAXPATH];
    int i, d, f;

    // create test tree
    snprintf(name, sizeof(name), "/tree%d", tid);
    printf("Create and test %s\n", name);
    size_t dlen = strlen(name);
    if (unfs_create(fs, name, 1, 0))
        FATAL("Create directory %s failed", name);

    // create depth level of directories and files
    for (d = 1; d <= tree_depth; d++) {
        // print progress
        //if (!verbose) fprintf(stderr, "%d.%-8d\r", tid, d);

        // create a temporary directory to be moved later
        snprintf(tmpname, sizeof(tmpname), "/tmp%d-dir%d", tid, d);
        VERBOSE("# add dir %s\n", tmpname);
        if (unfs_create(fs, tmpname, 1, 0))
            FATAL("Create directory %s failed", tmpname);

        // create a temporary file to be moved later
        snprintf(tmpname, sizeof(tmpname), "/tmp%d-file%d", tid, d);
        VERBOSE("# add file %s\n", tmpname);
        if (unfs_create(fs, tmpname, 0, 0))
            FATAL("Create file %s failed", tmpname);

        // create depth level directory
        snprintf(name + dlen, sizeof(name), "/dir%d", d);
        dlen = strlen(name);
        VERBOSE("# add dir %s\n", name);
        unfs_fd_t fd = unfs_dir_open(fs, name, UNFS_OPEN_CREATE);
        if (fd.error)
            FATAL("Create directory %s (%s)", name, strerror(fd.error));
        unfs_dir_close(fd);

        // create files in each directory
        u64 size;
        for (f = 1; f <= file_count; f++) {
            size = random() & 0xffff;
            snprintf(name + dlen, sizeof(name), "/file%d", f);
            VERBOSE("# add file %s %ld 1\n", name, size);
            fd = unfs_file_open(fs, name, UNFS_OPEN_CREATE);
            if (fd.error)
                FATAL("Create %s (%s)", name, strerror(fd.error));
            unfs_file_resize(fd, size, &f);
            unfs_file_close(fd);
        }

        // add segments to test and temp files alternately until
        // number of temp file segments are overflown and have to merge
        unfs_fd_t ftmp = unfs_file_open(fs, tmpname, 0);
        if (ftmp.error)
            FATAL("Open %s (%s)", tmpname, strerror(ftmp.error));
        int overflow = UNFS_MAXDS / file_count + 2;
        u64 tmpsize = 0;
        for (f = 1; f <= file_count; f++) {
            snprintf(name + dlen, sizeof (name), "/file%d", f);
            if (!small_file) {
                unfs_fd_t fd = unfs_file_open(fs, name, 0);
                if (fd.error)
                    FATAL("Open %s (%s)", name, strerror(fd.error));
                unfs_file_stat(fd, &size, 0, 0);

                for (i = 0; i < overflow; i++) {
                    int addsize = random() & 0xffff;
                    size += addsize;
                    unfs_file_resize(fd, size, &f);
                    tmpsize += addsize;
                    unfs_file_resize(ftmp, tmpsize, 0);
                }
                unfs_file_close(fd);
            }
            mark_file(name);
        }
        unfs_file_close(ftmp);

        // remove the first file
        snprintf(name + dlen, sizeof(name), "/file1");
        VERBOSE("# remove file %s\n", name);
        if (unfs_remove(fs, name, 0))
            FATAL("Remove file %s failed", name);

        // move a temporary directory over
        snprintf(tmpname, sizeof(tmpname), "/tmp%d-dir%d", tid, d);
        snprintf(name + dlen, sizeof(name), "/dir.%d.%d", tid, d);
        VERBOSE("# move dir %s %s\n", tmpname, name);
        if (unfs_rename(fs, tmpname, name, 0))
            FATAL("Move dir %s %s failed", tmpname, name);

        // move a temporary file over
        snprintf(tmpname, sizeof(tmpname), "/tmp%d-file%d", tid, d);
        snprintf(name + dlen, sizeof(name), "/file1x");
        VERBOSE("# move file %s %s\n", tmpname, name);
        if (unfs_rename(fs, tmpname, name, 0))
            FATAL("Move file %s %s failed", tmpname, name);
        mark_file(name);
    }
}

/**
 * Run a test thread.
 */
static void* test_thread(void* arg)
{
    int tid = (long)arg;

    // send ready and wait for signal to run test
    sem_post(&sm_ready);
    sem_wait(&sm_run);
    test_tree(tid);
    check_tree(tid);

    return 0;
}

/**
 * Main program.
 */
int main(int argc, char** argv)
{
    const char* prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];
    int opt, t;

    while ((opt = getopt(argc, argv, "n:t:d:f:vs")) != -1) {
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
        case 'd':
            tree_depth = atoi(optarg);
            if (tree_depth <= 0)
                FATAL("Tree depth must be > 0");
            break;
        case 'f':
            file_count = atoi(optarg);
            if (file_count <= 0)
                FATAL("File count must be > 0");
            break;
        case 's':
            small_file = 1;
            break;
        default:
            fprintf(stderr, usage, prog);
            exit(1);
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
    printf("UNFS COMPLEX TREE TEST BEGIN\n");

    // Format new UNFS filesystem
    printf("UNFS format device %s\n", device);
    if (unfs_format(device, prog, 0))
        FATAL("UNFS format failed");

    // Open to access UNFS filesystem
    printf("UNFS open device %s\n", device);
    fs = unfs_open(device);
    if (!fs)
        FATAL("UNFS open failed");

    // Spawn test threads
    printf("Test %d trees %d depths %d files per directory\n",
                                    thread_count, tree_depth, file_count);
    time_t tstart = time(0);
    sem_init(&sm_ready, 0, 0);
    sem_init(&sm_run, 0, 0);
    pthread_t* pts = calloc(thread_count, sizeof(pthread_t));
    for (t = 0; t < thread_count; t++) {
        pthread_create(&pts[t], 0, test_thread, (void*)(long)(t + 1));
        sem_wait(&sm_ready);
    }
    for (t = 0; t < thread_count; t++)
        sem_post(&sm_run);
    for (t = 0; t < thread_count; t++)
        pthread_join(pts[t], 0);
    free(pts);
    sem_destroy(&sm_run);
    sem_destroy(&sm_ready);
    unfs_close(fs);

    // Reopen filesystem and verify tree again
    printf("UNFS reopen device %s\n", device);
    fs = unfs_open(device);
    int isdir;
    u64 size;
    if (!unfs_exist(fs, "/", &isdir, &size))
        FATAL("/ does not exist");
    if (!isdir || size != thread_count)
        FATAL("/ size %ld expect %d", size, thread_count);
    if (!fs)
        FATAL("UNFS open failed");
    for (t = 1; t <= thread_count; t++)
        check_tree(t);

    // Verify filesystem info
    unfs_header_t hdr;
    printf("~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");
    if (unfs_stat(fs, &hdr, 1))
        FATAL("UNFS status error");
    printf("~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");
    unfs_close(fs);

    u64 exp = 1 + thread_count + (thread_count * (tree_depth * 2));
    if (hdr.dircount != exp)
        FATAL("Dir count %ld expect %#lx", hdr.dircount, exp);
    exp += thread_count * tree_depth * file_count;
    if (hdr.fdcount != exp)
        FATAL("FD count %#lx expect %#lx", hdr.fdcount, exp);
    exp = hdr.pagecount - (hdr.fdcount + hdr.delcount + 1) * UNFS_FILEPC;
    if (hdr.fdpage != exp)
        FATAL("FD page %#lx expect %#lx", hdr.fdpage, exp);

    if (unfs_check(device))
        return 1;
    printf("UNFS COMPLEX TREE TEST COMPLETE (%ld seconds)\n", time(0) - tstart);
    LOG_CLOSE();
    return 0;
}
