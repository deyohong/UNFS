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
 * @brief UNFS raw device specific implementation.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#include "unfs.h"
#include "unfs_log.h"


/// Raw device implementation global structure
typedef struct {
    char*                   device;         ///< device name
    u64                     blockcount;     ///< device block count
    u32                     blocksize;      ///< device block size
    int                     fd;             ///< device file descriptor
    unfs_header_t*          fsheader;       ///< filesystem header
} unfs_raw_dev_t;

/// Print fatal error message and terminate (unrecoverable error)
#define FATAL(fmt, arg...) do { ERROR(fmt, ##arg); unfs_cleanup(); abort(); \
                           } while (0)
void unfs_cleanup();

/// UNVMe global object
static unfs_raw_dev_t    dev;

/**
 * Open the raw device.
 * @param   device         device name
 * @return  allocated filesystem header or NULL upon failure.
 */
static unfs_header_t* unfs_dev_open(const char* device)
{
    DEBUG_FN("%s", device);
    if (dev.fsheader) return dev.fsheader;

    // open device and get size info
    dev.fd = open(device, O_RDWR|O_DIRECT);
    if (dev.fd < 0)
        FATAL("open %s (%s)", device, strerror(errno));
    if (ioctl(dev.fd, BLKGETSIZE, &dev.blockcount) < 0)
        FATAL("cannot get %s block count (%s)", device, strerror(errno));
    if (ioctl(dev.fd, BLKSSZGET, &dev.blocksize) < 0)
        FATAL("cannot get %s block size (%s)", device, strerror(errno));
    if (dev.blocksize > UNFS_PAGESIZE)
        FATAL("unsupported block size %d > %d", dev.blocksize, UNFS_PAGESIZE);
    dev.device = strdup(device);

    // calculate filesystem header based on disk capacity
    dev.blockcount /= (dev.blocksize / 512);
    u64 pagecount = dev.blockcount / (UNFS_PAGESIZE / dev.blocksize);
    int bitsperpage = 8 << UNFS_PAGESHIFT;
    u64 datapage = (pagecount + bitsperpage - 1) / bitsperpage + 1;

    // allocate filesystem header including the free map
    void* hp = mmap(0, datapage << UNFS_PAGESHIFT, PROT_READ|PROT_WRITE,
                    MAP_PRIVATE|MAP_ANONYMOUS|MAP_LOCKED, -1, 0);
    if (hp == MAP_FAILED)
        FATAL("mmap %lu pages failed", datapage);
    dev.fsheader = hp;
    dev.fsheader->blockcount = dev.blockcount;
    dev.fsheader->blocksize = dev.blocksize;
    dev.fsheader->pagecount = pagecount;
    dev.fsheader->pagesize = UNFS_PAGESIZE;
    dev.fsheader->datapage = datapage;

    return (dev.fsheader);
}

/**
 * Close the device and release resources.
 */
static void unfs_dev_close()
{
    DEBUG_FN();
    if (dev.device)
        free(dev.device);
    if (dev.fsheader)
        munmap(dev.fsheader, dev.fsheader->datapage << UNFS_PAGESHIFT);
    if (dev.fd > 0) close(dev.fd);
    memset(&dev, 0, sizeof(dev));
}

/**
 * Allocate device IO context.  Not applicable to raw device.
 * @return  0
 */
static unfs_ioc_t unfs_dev_ioc_alloc()
{
    return 0;
}

/**
 * Release device IO context.  Not applicable to raw device.
 * @param   ioc         IO context
 */
static void unfs_dev_ioc_free(unfs_ioc_t ioc)
{
}

/**
 * Allocate a buffer associated with the specified IO context.
 * @param   ioc         IO context
 * @param   pc          pointer to number of pages requested
 * @return  IO queue buffer.
 */
static void* unfs_dev_page_alloc(unfs_ioc_t ioc, u32* pc)
{
    if (*pc > 4096) *pc = 4096;
    void* buf = mmap(0, *pc << UNFS_PAGESHIFT, PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED)
        FATAL("mmap %u pages failed", *pc);
    return buf;
}

/**
 * Free an IO buffer associated with the specified IO context.
 * @param   ioc         IO context
 * @param   buf         IO buffer
 * @param   pc          number of pages to free
 */
static void unfs_dev_page_free(unfs_ioc_t ioc, void* buf, u32 pc)
{
    if (munmap(buf, pc << UNFS_PAGESHIFT))
        FATAL("munmap %p failed", buf);
}

/**
 * Do unvme_read, if failed just print an error and terminate.
 * @param   ioc         IO context
 * @param   buf         data buffer
 * @param   pa          page address
 * @param   pc          page count
 */
static void unfs_dev_read(unfs_ioc_t ioc, void* buf, u64 pa, u32 pc)
{
    DEBUG_FN("%#lx %#x", pa, pc);
    off_t off = pa << UNFS_PAGESHIFT;
    ssize_t size = pc << UNFS_PAGESHIFT;
    while (size) {
        ssize_t n = pread(dev.fd, buf, size, off);
        if (n < 0)
            FATAL("pread size %#lx off %#lx (%s)", size, off, strerror(errno));
        buf += n;
        off += n;
        size -= n;
    }
}

/**
 * Do unvme_write, if failed just print an error and terminate.
 * @param   ioc         IO context
 * @param   buf         data buffer
 * @param   pa          page address
 * @param   pc          page count
 */
static void unfs_dev_write(unfs_ioc_t ioc, const void* buf, u64 pa, u32 pc)
{
    DEBUG_FN("%#lx %#x", pa, pc);
    off_t off = pa << UNFS_PAGESHIFT;
    ssize_t size = pc << UNFS_PAGESHIFT;
    while (size) {
        ssize_t n = pwrite(dev.fd, buf, size, off);
        if (n < 0)
            FATAL("pwrite size %#lx off %#lx (%s)", size, off, strerror(errno));
        buf += n;
        off += n;
        size -= n;
    }
}

/**
 * Bind to raw device implementation.
 * @param   devfp        device function pointer
 * @param   device      device name
 * @return  device filesystem reference or NULL upon failure.
 */
unfs_header_t* unfs_raw_open(unfs_device_io_t* devfp, const char* device)
{
    devfp->close = unfs_dev_close;
    devfp->ioc_alloc = unfs_dev_ioc_alloc;
    devfp->ioc_free = unfs_dev_ioc_free;
    devfp->page_alloc = unfs_dev_page_alloc;
    devfp->page_free = unfs_dev_page_free;
    devfp->read = unfs_dev_read;
    devfp->write = unfs_dev_write;
    return unfs_dev_open(device);
}
