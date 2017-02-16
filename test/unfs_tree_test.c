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
          -f FILECOUNT    number of files per directory (default 16)\n\
          DEVICE_NAME     device name\n";

static unfs_fs_t    fs;                     ///< filesystem handle
static int          verbose = 0;            ///< verbose flag
static int          thread_count = 32;      ///< number of threads
static int          tree_depth = 8;         ///< directory tree depth
static int          file_count = 16;        ///< number of files per directory
static sem_t        sm_ready;               ///< thread ready semaphore
static sem_t        sm_run;                 ///< thread run test semaphore

/// Print if verbose flag is set
#define VERBOSE(fmt, arg...) if (verbose) printf(fmt, ##arg)


/**
 * Open a file and setup pattern.
 */
static unfs_fd_t prep_file(const char* filename, unfs_node_io_t* niop)
{
    unfs_fd_t fd = unfs_file_open(fs, filename, 0);
    if (fd.error)
        FATAL("Open %s (%s)", filename, strerror(fd.error));

    u64 size;
    u32 dsc;
    unfs_ds_t* dslp;
    unfs_file_stat(fd, &size, &dsc, &dslp);
    memset(niop, (int)size, sizeof(*niop));
    niop->node.size = size;
    niop->node.dscount = dsc;
    memcpy(niop->node.ds, dslp, dsc * sizeof(unfs_ds_t));
    strcpy(niop->name, filename);
    free(dslp);

    return fd;
}

/**
 * Mark a file by writing its node info at the end.
 */
static void mark_file(const char* filename)
{
    unfs_node_io_t niop;
    unfs_fd_t fd = prep_file(filename, &niop);
    VERBOSE("# mark %s %#lx %u\n", filename, niop.node.size, niop.node.dscount);

    u64 pos = 0;
    while (pos < niop.node.size) {
        u64 n = niop.node.size - pos;
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
    unfs_node_io_t niop;
    unfs_fd_t fd = prep_file(filename, &niop);
    VERBOSE("# check %s %#lx %u\n", filename, niop.node.size, niop.node.dscount);

    u64 pos = 0;
    unfs_node_io_t rniop;
    while (pos < niop.node.size) {
        u64 n = niop.node.size - pos;
        if (n > sizeof(rniop)) n = sizeof(rniop);
        unfs_file_read(fd, &rniop, pos, n);
        if (memcmp(&niop, &rniop, n))
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
        if (d < tree_depth) exp++;
        VERBOSE("# check %s has %ld children\n", name, exp);
        dlen = strlen(name);
        isdir = 0;
        size = 0;
        if (!unfs_exist(fs, name, &isdir, &size))
            FATAL("%s does not exist", name);
        if (!isdir || size != exp)
            FATAL("%s size %ld expect %ld", name, size, exp);
        for (f = 1; f <= file_count; f++) {
            snprintf(name + dlen, sizeof (name), "/file%d", f);
            if (f == 1) strcat(name, "x");
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
    int d, f;

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
        VERBOSE("# create dir %s\n", tmpname);
        if (unfs_create(fs, tmpname, 1, 0))
            FATAL("Create directory %s failed", tmpname);

        // create a temporary file to be moved later
        snprintf(tmpname, sizeof(tmpname), "/tmp%d-dir%d-file", tid, d);
        VERBOSE("# create file %s\n", tmpname);
        if (unfs_create(fs, tmpname, 0, 0))
            FATAL("Create file %s failed", tmpname);

        // create depth level directory
        snprintf(name + dlen, sizeof(name), "/dir%d", d);
        dlen = strlen(name);
        VERBOSE("# create dir %s\n", name);
        int err = unfs_create(fs, name, 1, 1);
        if (err)
            FATAL("Create directory %s (%s)", name, strerror(err));

        // create files in each directory
        u64 size;
        for (f = 1; f <= file_count; f++) {
            size = random() & 0xffff;
            snprintf(name + dlen, sizeof(name), "/file%d", f);
            VERBOSE("# create file %s %ld 1\n", name, size);
            unfs_fd_t fd = unfs_file_open(fs, name, UNFS_OPEN_CREATE);
            if (fd.error)
                FATAL("Create %s (%s)", name, strerror(fd.error));
            unfs_file_resize(fd, size, &f);
            unfs_file_close(fd);
        }

        // keep resizing until file segments are merged
        unfs_fd_t ftmp = unfs_file_open(fs, tmpname, 0);
        if (ftmp.error)
            FATAL("Open %s (%s)", tmpname, strerror(ftmp.error));
        u64 tmpsize = 0;
        u32 dsc = 0, dsm;
        for (f = 1;;) {
            snprintf(name + dlen, sizeof (name), "/file%d", f);
            unfs_fd_t fd = unfs_file_open(fs, name, 0);
            if (fd.error)
                FATAL("Open %s (%s)", name, strerror(fd.error));
            unfs_file_stat(fd, &size, 0, 0);
            int addsize = random() & 0xffff;
            size += addsize;
            unfs_file_resize(fd, size, &f);
            unfs_file_close(fd);

            tmpsize += addsize;
            unfs_file_resize(ftmp, tmpsize, 0);
            unfs_file_stat(ftmp, &size, &dsm, 0);
            if (dsm < dsc) break;
            dsc = dsm;
            if (++f > file_count) f = 1;
        }
        while (f-- > 0) {
            tmpsize += dsc * f;
            unfs_file_resize(ftmp, tmpsize, 0);
            unfs_file_stat(ftmp, &size, &dsm, 0);
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
        VERBOSE("# rename dir %s %s\n", tmpname, name);
        if (unfs_rename(fs, tmpname, name, 0))
            FATAL("Move dir %s %s failed", tmpname, name);

        // move a temporary file over
        snprintf(tmpname, sizeof(tmpname), "/tmp%d-dir%d-file", tid, d);
        snprintf(name + dlen, sizeof(name), "/file1x");
        VERBOSE("# rename file %s %s\n", tmpname, name);
        if (unfs_rename(fs, tmpname, name, 0))
            FATAL("Move file %s %s failed", tmpname, name);

        // write to each file with unique data
        for (f = 1; f <= file_count; f++) {
            snprintf(name + dlen, sizeof (name), "/file%d", f);
            if (f == 1) strcat(name, "x");
            mark_file(name);
        }
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

    while ((opt = getopt(argc, argv, "n:t:d:f:v")) != -1) {
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
        default:
            fprintf(stderr, usage, prog);
            exit(1);
        }
    }

    const char* device = getenv("UNFS_DEVICE");
    if ((optind + 1) == argc) device = argv[optind++];
    if (!device || optind != argc) {
        fprintf(stderr, usage, prog);
        exit(1);
    }

    LOG_OPEN();
    printf("UNFS COMPLEX TREE TEST BEGIN\n");

    // Format new UNFS filesystem
    printf("UNFS format device %s\n", device);
    if (unfs_format(device, prog, verbose))
        FATAL("UNFS format failed");

    // Open to access UNFS filesystem
    printf("UNFS open device %s\n", device);
    fs = unfs_open(device);
    if (!fs)
        FATAL("UNFS open failed");

    // Spawn test threads
    printf("Test %d trees %d directories %d files per directory\n",
                                    thread_count, tree_depth, file_count);
    time_t tstart = time(0);
    sem_init(&sm_ready, 0, 0);
    sem_init(&sm_run, 0, 0);
    pthread_t* pts = calloc(thread_count, sizeof(pthread_t));
    for (t = 0; t < thread_count; t++) {
        pthread_create(&pts[t], 0, test_thread, (void*)(long)(t + 1));
        sem_wait(&sm_ready);
    }
    for (t = 0; t < thread_count; t++) sem_post(&sm_run);
    for (t = 0; t < thread_count; t++) pthread_join(pts[t], 0);
    free(pts);
    sem_destroy(&sm_run);
    sem_destroy(&sm_ready);
    unfs_close(fs);

    // Reopen filesystem and verify tree again
    printf("UNFS reopen device %s\n", device);
    fs = unfs_open(device);
    if (!fs)
        FATAL("UNFS open failed");
    int isdir;
    u64 size;
    if (!unfs_exist(fs, "/", &isdir, &size))
        FATAL("/ does not exist");
    if (!isdir || size != thread_count)
        FATAL("/ size %ld expect %d", size, thread_count);
    if (!fs)
        FATAL("UNFS open failed");
    for (t = 1; t <= thread_count; t++) check_tree(t);

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
    if (hdr.fdnextpage != exp)
        FATAL("FD next %#lx expect %#lx", hdr.fdnextpage, exp);

    if (unfs_check(device)) return 1;
    printf("UNFS COMPLEX TREE TEST COMPLETE (%ld seconds)\n", time(0) - tstart);
    LOG_CLOSE();
    return 0;
}
