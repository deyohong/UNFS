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
 * @brief UNFS UNVMe device specific implementation.
 */

#include <string.h>
#include <stdlib.h>
#include <sched.h>
#include <errno.h>

#include "unfs.h"
#include "unfs_log.h"

#ifdef UNFS_UNVME
#include <unvme.h>

/// UNVMe device implementation global structure
typedef struct {
    char*                   device;         ///< device name
    const unvme_ns_t*       ns;             ///< UNVME namespace handle
    int                     pbshift;        ///< page to block shift
    u64                     qiocmask;       ///< IO queue available mask
    u64                     qbufmask;       ///< IO queue buffer used mask
    u64                     qallmask;       ///< IO queue all mask
    int                     qnext;          ///< next IO queue to check
    int                     qpac;           ///< IO queue allocated page count
    void**                  qbuf;           ///< IO queue allocated pages
    unfs_header_t*          fsheader;       ///< filesystem header
} unfs_unvme_dev_t;

/// Print fatal error message and terminate (unrecoverable error)
#define FATAL(fmt, arg...) do { ERROR(fmt, ##arg); unfs_cleanup(); abort(); \
                           } while (0)
void unfs_cleanup();

/// UNVMe global object
static unfs_unvme_dev_t    dev;


/**
 * Open the device and allocate IO memory for header and data.
 * @param   device      device name
 * @return  a filesystem reference or NULL upon failure.
 */
static unfs_header_t* unfs_dev_open(const char* device)
{
    DEBUG_FN("%s", device);
    if (dev.fsheader) return dev.fsheader;

    // use environment variables to pass device specific values
    char* env = getenv("UNFS_NSID");
    int nsid = env ? atoi(env) : 1;
    env = getenv("UNFS_QCOUNT");
    int qcount = env ? atoi(env) : 24;
    env = getenv("UNFS_QDEPTH");
    int qdepth = env ? atoi(env) : 256;
    env = getenv("UNFS_QPAC");
    int qpac = env ? atoi(env) : 4096;

    // limit max qcount to a few bits less than the allocated mask size
    if (qcount > 60)
        FATAL("only support max of UNFS_QCOUNT 60");
    dev.qallmask = (u64)-1L >> (64 - qcount);

    // open NVMe device
    const unvme_ns_t* ns = unvme_open(device, nsid, qcount, qdepth);
    if (!ns)
        FATAL("unvme_open %s", device);
    if (ns->pagesize != UNFS_PAGESIZE)
        FATAL("unsupported page size %u", ns->pagesize);
    dev.device = strdup(device);
    dev.ns = ns;
    dev.pbshift = ns->pageshift - ns->blockshift;
    u64 pagecount = ns->blockcount / ns->nbpp;
    int bitsperpage = 8 << UNFS_PAGESHIFT;
    u64 datapage = (pagecount + bitsperpage - 1) / bitsperpage + 1;

    // allocate filesystem header including the bitmap
    dev.fsheader = unvme_alloc(ns, datapage << UNFS_PAGESHIFT);
    if (!dev.fsheader)
        FATAL("unvme_alloc header %lu pages", datapage);
    dev.fsheader->blockcount = ns->blockcount;
    dev.fsheader->blocksize = ns->blocksize;
    dev.fsheader->pagecount = pagecount;
    dev.fsheader->pagesize = ns->pagesize;
    dev.fsheader->datapage = datapage;

    // setup IO queues and memory
    dev.qpac = qpac;
    dev.qbuf = calloc(qcount, sizeof(void*));
    dev.qbuf[0] = unvme_alloc(ns, (u64)(qcount * qpac) << UNFS_PAGESHIFT);
    if (!dev.qbuf[0])
        FATAL("unvme_alloc %u pages", qcount * qpac);
    int q;
    for (q = 1; q < qcount; q++)
        dev.qbuf[q] = dev.qbuf[q-1] + (qpac << UNFS_PAGESHIFT);

    return dev.fsheader;
}

/**
 * Close the device and release resources.
 */
static void unfs_dev_close()
{
    DEBUG_FN();
    if (dev.ns) {
        if (dev.qbuf) {
            if (dev.qbuf[0]) unvme_free(dev.ns, dev.qbuf[0]);
            free(dev.qbuf);
        }
        if (dev.device)
            free(dev.device);
        if (dev.fsheader)
            unvme_free(dev.ns, dev.fsheader);
        unvme_close(dev.ns);
    }
    memset(&dev, 0, sizeof(dev));
}

/**
 * Allocate a UNVMe IO context (i.e. NVMe queue) for thread exclusive use.
 * @return  IO context
 */
static unfs_ioc_t unfs_dev_ioc_alloc()
{
    int q = dev.qnext;
    for (;;) {
        if (++q >= dev.ns->qcount)
            q = 0;
        u64 mask = 1L << q;
        u64 qiocmask = __sync_fetch_and_or(&dev.qiocmask, mask);
        if ((qiocmask & mask) == 0L)
            break;
        if (dev.qiocmask == dev.qallmask)
            sched_yield();
    }
    dev.qnext = q;
    return (unfs_ioc_t)q;
}

/**
 * Release a UNVMe IO context (i.e. NVMe queue).
 * @param   ioc         IO context
 */
static void unfs_dev_ioc_free(unfs_ioc_t ioc)
{
    u64 mask = 1L << ioc;
    u64 qiocmask = __sync_fetch_and_and(&dev.qiocmask, ~mask);
    if ((qiocmask & mask) == 0L)
        FATAL("q%ld was not allocated", ioc);
}

/**
 * Allocate a buffer associated with the specified IO context.
 * @param   ioc         IO context
 * @param   pc          pointer to number of pages requested
 * @return  IO queue buffer.
 */
static void* unfs_dev_page_alloc(unfs_ioc_t ioc, u32* pc)
{
    // just need to support 1 allocation request per queue for now
    u64 mask = 1L << ioc;
    u64 qbufmask = __sync_fetch_and_or(&dev.qbufmask, mask);
    if ((qbufmask & mask) != 0L)
        FATAL("q%ld buffer is already allocated", ioc);
    if (*pc > dev.qpac)
        *pc = dev.qpac;
    return dev.qbuf[ioc];
}

/**
 * Free an IO buffer associated with the specified IO context.
 * @param   ioc         IO context
 * @param   buf         IO buffer
 * @param   pc          number of pages to free
 */
static void unfs_dev_page_free(unfs_ioc_t ioc, void* buf, u32 pc)
{
    if (buf != dev.qbuf[ioc])
        FATAL("bad q%ld buffer", ioc);
    u64 mask = 1L << ioc;
    u64 qbufmask = __sync_fetch_and_and(&dev.qbufmask, ~mask);
    if ((qbufmask & mask) == 0L)
        FATAL("q%ld buffer is already freed", ioc);
}

/**
 * Do unvme_read, if failed just print an error and terminate.
 * @param   ioc         IO context
 * @param   buf         data buffer
 * @param   pa          page address
 * @param   pc          page count
 * 
 */
static void unfs_dev_read(unfs_ioc_t ioc, void* buf, u64 pa, u32 pc)
{
    DEBUG_FN("q%ld %#lx %#x", ioc, pa, pc);
    if (unvme_read(dev.ns, ioc, buf, pa << dev.pbshift, pc << dev.pbshift))
         FATAL("unvme_read q%ld %#lx %u", ioc, pa, pc);
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
    DEBUG_FN("q%ld %#lx %#x", ioc, pa, pc);
    if (unvme_write(dev.ns, ioc, buf, pa << dev.pbshift, pc << dev.pbshift))
         FATAL("unvme_write q%ld %#lx %u", ioc, pa, pc);
}

#endif  // UNFS_UNVME

/**
 * Bind to UNVMe device implementation and open the device.
 * @param   devfp       device function pointer
 * @param   device      device name
 * @return  device filesystem reference or NULL upon failure.
 */
unfs_header_t* unfs_unvme_open(unfs_device_io_t* devfp, const char* device)
{
#ifdef UNFS_UNVME
    devfp->close = unfs_dev_close;
    devfp->ioc_alloc = unfs_dev_ioc_alloc;
    devfp->ioc_free = unfs_dev_ioc_free;
    devfp->page_alloc = unfs_dev_page_alloc;
    devfp->page_free = unfs_dev_page_free;
    devfp->read = unfs_dev_read;
    devfp->write = unfs_dev_write;
    return unfs_dev_open(device);
#else
    ERROR("No UNVMe support");
    return NULL;
#endif  // UNFS_UNVME
}

