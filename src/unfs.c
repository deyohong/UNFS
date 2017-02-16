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
 * @brief UNFS filesystem implementation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <search.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "unfs.h"
#include "unfs_log.h"


/// Convert byte length into page count
#define PAGECOUNT(len)      (((len) + UNFS_PAGESIZE - 1) >> UNFS_PAGESHIFT)

/// Check for filesystem context error
#define FS_CHECK(fs)        ((fs >> 16) != (unfs.fsid >> 16))

/// Filesystem write lock
#define FS_WRLOCK()         pthread_rwlock_wrlock(&unfs.lock)

/// Filesystem read lock
#define FS_RDLOCK()         pthread_rwlock_rdlock(&unfs.lock)

/// Filesystem try write lock
#define FS_TRYLOCK()        pthread_rwlock_trywrlock(&unfs.lock)

/// Filesystem unlock
#define FS_UNLOCK()         pthread_rwlock_unlock(&unfs.lock)

/// File write lock
#define FILE_WRLOCK(fp)     pthread_rwlock_wrlock(&fp->lock)

/// File read lock
#define FILE_RDLOCK(fp)     pthread_rwlock_rdlock(&fp->lock)

/// File unlock
#define FILE_UNLOCK(fp)     pthread_rwlock_unlock(&fp->lock)


/// Tree node as defined in tsearch.c (for custom tree walk function)
struct tnode {
    const void*             key;            ///< tsearch object handle
    struct tnode*           left;           ///< tsearch left node
    struct tnode*           right;          ///< tsearch right node
    unsigned int            red:1;          ///< tsearch red/black flag
};

/// Filesystem management structure
typedef struct {
    unfs_header_t*          header;         ///< filesystem header
    u64                     mapnext;        ///< next bitmap free index
    u64                     mapsynclo;      ///< map sync low page address
    u64                     mapsynchi;      ///< map sync high page address
    u64                     mapsyncfdlo;    ///< map sync low fd address
    u64                     mapsyncfdhi;    ///< map sync high fd address
    u64                     fsid;           ///< filesystem id to check
    int                     open;           ///< filesystem open count
    void*                   root;           ///< filesystem tree
    pthread_rwlock_t        lock;           ///< filesystem tree access lock
    unfs_device_io_t        dev;            ///< device implmentation
} unfs_filesystem_t;

/// UNFS static data object
static unfs_filesystem_t    unfs;

/// UNFS initialize/cleanup lock
static pthread_mutex_t      unfslock = PTHREAD_MUTEX_INITIALIZER;


/**
 * Print UNFS header status info.
 * @param   hp          filesystem header pointer
 */
void unfs_print_header(unfs_header_t* hp)
{
    printf("Label:       %s\n",   hp->label);
    printf("Version:     %s\n",   hp->version);
    printf("Block count: %#lx\n", hp->blockcount);
    printf("Block size:  %#x\n",  hp->blocksize);
    printf("Page count:  %#lx\n", hp->pagecount);
    printf("Page size:   %#x\n",  hp->pagesize);
    printf("Page free:   %#lx\n", hp->pagefree);
    printf("Data page:   %#lx\n", hp->datapage);
    printf("FD next:     %#lx\n", hp->fdnextpage);
    printf("FD count:    %#lx\n", hp->fdcount);
    printf("Dir count:   %#lx\n", hp->dircount);
    printf("Del count:   %#x\n",  hp->delcount);
    printf("Del max:     %#x\n",  hp->delmax);
    printf("Map size:    %#lx\n", hp->mapsize);
}

/**
 * Print out all node names in a tree (for debugging purpose).
 * @param   root        root node
 */
void unfs_print_tree(struct tnode* root)
{
    if (!root) return;
    unfs_node_t* nodep = (unfs_node_t*)(root->key);
    char* type = nodep->isdir ? "DIR" : "FILE";

    if (root->left == NULL && root->right == NULL) {
        printf("%s: %s %#lx\n", type, nodep->name, nodep->pageid);
    } else {
        if (root->left != NULL) unfs_print_tree(root->left);
        printf("%s: %s %#lx\n", type, nodep->name, nodep->pageid);
        if (root->right != NULL) unfs_print_tree(root->right);
    }
}

/**
 * Scan the whole bitmap array and count the number of bits set.
 * @return  number of pages are being use.
 */
static u64 unfs_map_count()
{
    u64 mapuse = 0, i;
    u64* map = (u64*)unfs.header->map;
    for (i = 0; i < unfs.header->mapsize; i++) {
        mapuse += __builtin_popcountl(*map);
        map++;
    }
    return mapuse;
}

/**
 * Check that the specified page addresses are set in the bit map.
 * @param   pageid      page address
 * @param   pagecount   number of pages
 * @return  0 if ok else 1.
 */
static int unfs_map_check(u64 pageid, u32 pagecount)
{
    if (pageid < unfs.header->datapage || pageid >= unfs.header->pagecount)
        return 1;
    u64* map = (u64*)unfs.header->map;
    pageid -= unfs.header->datapage;
    while (pagecount--) {
        if (!(map[pageid >> 6] & (1L << (63 - (pageid & 63))))) return 1;
        pageid++;
    }
    return 0;
}

/**
 * Allocate a contiguous number of free disk pages.
 * @param   pagecount   number of pages
 * @return  the page address or 0 if out of disk space.
 */
static u64 unfs_map_alloc(u32 pagecount)
{
    DEBUG_FN("%u", pagecount);

    u64 mapidx, pageid;
    int mapbit;

    u64 i = unfs.mapnext;
    u64* map = (u64*)unfs.header->map + i;
    u64 mapend = unfs.header->mapsize - (unfs.header->fdcount >> 5) - 1;

    if (pagecount < 64) {
        // check for contiguous free bits within a word
        for (; i < mapend; i++, map++) {
            u64 mask = *map;

            // check trailing zeros
            int tz = 64;
            if (mask) tz = __builtin_ctzl(mask);
            if (tz >= pagecount) {
                mapidx = i;
                mapbit = 64 - tz;
                // mark the bits
                *map |= (u64)(-1L << (64 - pagecount)) >> mapbit;
                goto found;
            }

            // check middle zeros
            int bc = 64 - tz;
            if ((bc - __builtin_popcountl(mask)) >= pagecount) {
                for (mapbit = 0; ;) {
                    int lz = __builtin_clzl(mask);
                    if (lz >= pagecount) {
                        mapidx = i;
                        // mark the bits
                        *map |= (u64)(-1L << (64 - pagecount)) >> mapbit;
                        goto found;
                    }
                    mapbit += lz + 1;
                    if (mapbit >= bc) break;
                    mask <<= lz + 1;
                }
            }
        }

    } else {
        int n = 0;
        int nzw = (pagecount + 63) >> 6;

        // check for contiguous clear words
        for (mapbit = 0; i < mapend; i++, map++) {
            if (*map) {
                n = 0;
            } else {
                if (n == 0) mapidx = i;
                if (++n == nzw) {
                    // mark the bits
                    *map |= -1L << ((64 - (pagecount & 63)) & 63);
                    while (--n > 0) *(--map) = -1L;
                    goto found;
                }
            }
        }
    }

    return 0;

found:
    unfs.header->pagefree -= pagecount;
    pageid = unfs.header->datapage + (mapidx << 6) + mapbit;

    if (unfs.mapsynclo > pageid) unfs.mapsynclo = pageid;
    u64 pagehi = pageid + pagecount;
    if (unfs.mapsynchi < pagehi) unfs.mapsynchi = pagehi;

    return pageid;
}

/**
 * Free up a contiguous number of disk pages.
 * @param   pageid      page address
 * @param   pagecount   number of pages
 */
static void unfs_map_free(u64 pageid, u32 pagecount)
{
    DEBUG_FN("%#lx %u", pageid, pagecount);
    u64 pa = pageid - unfs.header->datapage;
    u64 i = pa >> 6;
    u64* map = (u64*)unfs.header->map + i;
    if (unfs.mapnext > i) unfs.mapnext = i;

    // a file segment may be allocated contiguously by multiple unfs_map_alloc
    // so the free scheme is not exactly the same as allocation
    u32 pc = pagecount;  
    int b = pa & 63;
    if (b > 0) {
        int rem = 64 - b;
        if (pc > rem) pc = rem;
        u64 mask = (u64)(-1L << (64 - pc)) >> b;
        if ((*map & mask) != mask)
            FATAL("%u map[%#lx]=0x%016lx bits %d-%d not set",
                  pagecount, i, *map, b, b + pc - 1);
        *map &= ~mask;
        pc = pagecount - pc;
        i++;
        map++;
    }

    for (; pc > 0; i++, map++) {
        if (pc < 64) {
            u64 mask = -1L << (64 - pc);
            if ((*map & mask) != mask)
                FATAL("%u map[%#lx]=0x%016lx bits 0-%d not set",
                      pagecount, i, *map, pc - 1);
            *map &= ~mask;
            break;
        }
        if (*map != -1L)
            FATAL("%u map[%#lx]=0x%016lx bits 0-63 not set",
                  pagecount, i, *map);
        *map = 0L;
        pc -= 64;
    }

    unfs.header->pagefree += pagecount;

    if (unfs.mapsynclo > pageid) unfs.mapsynclo = pageid;
    pageid += pagecount;
    if (unfs.mapsynchi < pageid) unfs.mapsynchi = pageid;
}

/**
 * Check if the first path name is the child of the second path name.
 * @param   child       child path name
 * @param   parent      parent path name
 * @return  1 if ok else 0.
 */
static int unfs_child_of(const char* child, const char* parent)
{
    int clen = strlen(child);
    int plen = strlen(parent);
    if (clen <= plen) return 0;
    if (plen == 1) return (strchr(child + 1, '/') == 0);
    const char* s = child + plen;
    return (*s == '/' && !strncmp(child, parent, plen) && !strchr(s + 1, '/'));
}

/**
 * Tree function to compare two file nodes by name.
 * @param   f1          file 1
 * @param   f2          file 2
 * @return  strcmp of filenames result.
 */
static int unfs_node_cmp_fn(const void* f1, const void* f2)
{
    return strcmp(((unfs_node_t*)f1)->name, ((unfs_node_t*)f2)->name);
}

/**
 * Allocate a new disk file entry.
 * @param   dir         directory flag
 * @return  page address for the new node or 0 if failed.
 */
static u64 unfs_node_alloc(int dir)
{
    u64 fdpage;

    // if deleted stack has entry then use it
    // otherwise use the next fdpage entry
    if (unfs.header->delcount) {
        fdpage = unfs.header->delstack[--unfs.header->delcount];
    } else {
        // mark the page bits
        fdpage = unfs.header->fdnextpage;
        u64 pa = fdpage - unfs.header->datapage;
        u64* map = (u64*)unfs.header->map + (pa >> 6);
        u64 mask = (u64)(-1L << (64 - UNFS_FILEPC)) >> (pa & 63);
        if ((*map & mask) != 0) {
            ERROR("cannot allocate space for a new file/dir entry");
            return 0;
        }
        *map |= mask;

        unfs.header->pagefree -= UNFS_FILEPC;
        unfs.header->fdnextpage -= UNFS_FILEPC;

        if (unfs.mapsyncfdlo > fdpage) unfs.mapsyncfdlo = fdpage;
        u64 fdhi = fdpage + UNFS_FILEPC;
        if (unfs.mapsyncfdhi < fdhi) unfs.mapsyncfdhi = fdhi;
    }

    unfs.header->fdcount++;
    if (dir) unfs.header->dircount++;

    return fdpage;
}

/**
 * Free a disk file entry.
 * @param   nodep       file node
 * @return  0 if done, else 1 indicates slot is vacant and pending.
 */
static int unfs_node_free(unfs_node_t* nodep)
{
    int pending = 0;

    // if deleted stack is not full then put the node page on it
    // otherwise move fdpage forward and let the caller move the file entry
    if (unfs.header->delcount < unfs.header->delmax) {
        unfs.header->delstack[unfs.header->delcount++] = nodep->pageid;
    } else {
        unfs.header->fdnextpage += UNFS_FILEPC;
        unfs.header->pagefree += UNFS_FILEPC;

        u64 fdpage = unfs.header->fdnextpage;
        if (nodep->pageid != fdpage) pending = 1;

        // clear the page bits
        u64 pa = fdpage - unfs.header->datapage;
        u64* map = (u64*)unfs.header->map + (pa >> 6);
        u64 mask = (u64)(-1L << (64 - UNFS_FILEPC)) >> (pa & 63);
        if ((*map & mask) != mask)
            FATAL("%s page %#lx bits not set", nodep->name, nodep->pageid);
        *map &= ~mask;

        if (unfs.mapsyncfdlo > fdpage) unfs.mapsyncfdlo = fdpage;
        fdpage += UNFS_FILEPC;
        if (unfs.mapsyncfdhi < fdpage) unfs.mapsyncfdhi = fdpage;
    }

    unfs.header->fdcount--;
    if (nodep->isdir) unfs.header->dircount--;
    return pending;
}

/**
 * Sync a node to device.
 * @param   ioc         io context
 * @param   nodep       file node
 */
static void unfs_node_sync(unfs_ioc_t ioc, unfs_node_t* nodep)
{
    DEBUG_FN("%s page=%#lx size=%#lx dsc=%u",
             nodep->name, nodep->pageid, nodep->size, nodep->dscount);

    u32 iopc = UNFS_FILEPC;
    unfs_node_io_t* niop = unfs.dev.page_alloc(ioc, &iopc);
    if (iopc != UNFS_FILEPC)
        FATAL("cannot allocate %d pages", UNFS_FILEPC);
    memset(&niop->node, 0, sizeof(unfs_node_t));
    niop->node.pageid = nodep->pageid;
    niop->node.parentid = nodep->parentid;
    niop->node.size = nodep->size;
    niop->node.isdir = nodep->isdir;
    niop->node.dscount = nodep->dscount;
    if (!nodep->isdir)
        memcpy(niop->node.ds, nodep->ds, nodep->dscount * sizeof(unfs_ds_t));
    strcpy(niop->name, nodep->name);
    unfs.dev.write(ioc, niop, nodep->pageid, UNFS_FILEPC);
    unfs.dev.page_free(ioc, niop, iopc);
}

/**
 * Find a node by its canonical name starting at the specified root node.
 * @param   name        canonical name to search
 * @return  node pointer or NULL if not found.
 */
static unfs_node_t* unfs_node_find(const char* name)
{
    unfs_node_t key = { .name = (char*)name };
    unfs_node_t** pp = tfind(&key, &unfs.root, unfs_node_cmp_fn);
    DEBUG_FN("%s %#lx", name, pp ? (*pp)->pageid : 0);
    return pp ? *pp : NULL;
}

/**
 * Find the parent of the specified node by its canonical name.
 * @param   name        canonical name to search
 * @return  parent node pointer or NULL if not found.
 */
static unfs_node_t* unfs_node_find_parent(const char* name)
{
    DEBUG_FN("%s", name);
    const char* s = strrchr(name, '/');
    if (!s) return NULL;
    char path[UNFS_MAXPATH];
    size_t len = (s == name) ? 1 : s - name;
    strncpy(path, name, sizeof(path));
    path[len] = 0;
    return unfs_node_find(path);
}

/**
 * Walk the tree to update the parent id of all the children of the specified
 * parent node.
 * @param   root        root node
 * @param   parent      parent node
 * @param   ioc         io context
 */
static void unfs_node_update_children(unfs_ioc_t ioc,
                                      struct tnode* root, unfs_node_t* parent)
{
    if (!root) return;
    unfs_node_t* nodep = (unfs_node_t*)(root->key);

    if (root->left == NULL && root->right == NULL) {
        if (nodep->parent == parent) {
            FILE_WRLOCK(nodep);
            nodep->parentid = parent->pageid;
            unfs_node_sync(ioc, nodep);
            FILE_UNLOCK(nodep);
        }
    } else {
        if (root->left != NULL) {
            unfs_node_update_children(ioc, root->left, parent);
        }

        if (nodep->parent == parent) {
            FILE_WRLOCK(nodep);
            nodep->parentid = parent->pageid;
            unfs_node_sync(ioc, nodep);
            FILE_UNLOCK(nodep);
        }

        if (root->right != NULL) {
            unfs_node_update_children(ioc, root->right, parent);
        }
    }
}

/**
 * Remove and free a disk file entry with its associated data pages.
 * @param   ioc         io context
 * @param   nodep       node file pointer
 */
static void unfs_node_remove(unfs_ioc_t ioc, unfs_node_t* nodep)
{
    DEBUG_FN("%s %#lx", nodep->name, nodep->pageid);
    // delete the node from the tree and update the parent size
    tdelete(nodep, &unfs.root, unfs_node_cmp_fn);
    nodep->parent->size--;
    unfs_node_sync(ioc, nodep->parent);

    // free up file data segments
    if (!nodep->isdir) {
        int i;
        for (i = 0; i < nodep->dscount; i++) {
            unfs_map_free(nodep->ds[i].pageid, nodep->ds[i].pagecount);
        }
    }

    // if free up the node is still pending, this requires moving the last
    // file entry into the deleted slot and if it's a directory then 
    // all its children nodes have to be updated.
    if (unfs_node_free(nodep) != 0) {
        INFO("WARN: delete stack is full, %s location is moved", nodep->name);
        u32 iopc = UNFS_FILEPC;
        unfs_node_io_t* niop = unfs.dev.page_alloc(ioc, &iopc);
        unfs.dev.read(ioc, niop, unfs.header->fdnextpage, UNFS_FILEPC);
        unfs_node_t* lastnode = unfs_node_find(niop->name);
        lastnode->pageid = nodep->pageid;
        niop->node.pageid = nodep->pageid;
        unfs.dev.write(ioc, niop, nodep->pageid, UNFS_FILEPC);
        if (nodep->isdir)
            unfs_node_update_children(ioc, unfs.root, lastnode);
        unfs.dev.page_free(ioc, niop, iopc);
    }
    free(nodep);
}

/**
 * Add a new node under the specified parent node in the tree.
 * @param   parent   parent node
 * @param   nodep    node to add
 * @return  pointer to the newly added node.
 */
static unfs_node_t* unfs_node_add(unfs_node_t* parent, const unfs_node_t* nodep)
{
    DEBUG_FN("%s %#lx %#lx", nodep->name, nodep->pageid, nodep->parentid);
    size_t len = strlen(nodep->name);

    // validate a filename
    char* s;
    for (s = nodep->name + 1; isprint(*s); s++);
    if (*s || nodep->name[0] != '/' || (len > 1 && nodep->name[len-1] == '/')) {
        ERROR("invalid name %s", nodep->name);
        return NULL;
    }

    // validate parent
    if (parent) {
        if (!unfs_child_of(nodep->name, parent->name))
            FATAL("%s is not the parent of %s", parent->name, nodep->name);
        if (nodep->pageid && parent->pageid && nodep->parentid != parent->pageid)
            FATAL("%s %#lx not matched parent %s %#lx", nodep->name,
                            nodep->parentid, parent->name, parent->pageid);
    }

    size_t nsize = nodep->isdir ? sizeof(unfs_node_t) : UNFS_PAGESIZE;
    size_t memsize = nsize + len + 1;
    unfs_node_t* newnodep = malloc(memsize);
    pthread_rwlock_init(&newnodep->lock, NULL);
    newnodep->name = (char*)newnodep + nsize;
    strcpy(newnodep->name, nodep->name);
    newnodep->parent = parent;
    newnodep->memsize = memsize;
    newnodep->open = 0;
    newnodep->pageid = nodep->pageid;
    newnodep->parentid = nodep->parentid;
    newnodep->size = nodep->size;
    newnodep->isdir = nodep->isdir;
    newnodep->dscount = nodep->dscount;
    if (!nodep->isdir) {
        memcpy(newnodep->ds, nodep->ds, newnodep->dscount * sizeof(unfs_ds_t));
    }
    tsearch(newnodep, &unfs.root, unfs_node_cmp_fn);

    return newnodep;
}

/**
 * Add all the parent directories (if not exists) of the specified node name.
 * This function allows setting nodes upon initialization where file entries
 * are read from disk that are layed out in random order.  Parent nodes
 * that are created in advance will be uninitialized.
 * @param   name        canonical name
 * @return  the last created/existed parent node.
 */
static unfs_node_t* unfs_node_add_parents(const char* name)
{
    DEBUG_FN("%s", name);
    char path[UNFS_MAXPATH] = "";
    unfs_node_t node = { .name = path, .isdir = 1, 0 };

    unfs_node_t* parent = NULL;
    name++;
    char* next;
    while ((next = strchr(name, '/')) != NULL) {
        if (next[1] == 0) break;
        int len = next - name + 1;
        strncat(path, name - 1, len);
        name = next + 1;

        unfs_node_t* found = unfs_node_find(path);
        if (found) parent = found;
        else parent = unfs_node_add(parent, &node);
    }
    if (!parent) parent = unfs_node_find("/");
    return parent;
}

/**
 * Read/Write file data to/from device (all data pages have been allocated).
 * @param   ioc         io context
 * @param   nodep       file node pointer
 * @param   buf         data buffer
 * @param   offset      file offset position
 * @param   len         number of bytes
 * @param   wflag       write flag
 * @return  0 if ok else error code.
 */
static int unfs_node_rw(unfs_ioc_t ioc, unfs_node_t* nodep,
                        void* buf, u64 offset, u64 len, int wflag)
{
    if (len == 0) return 0;

    // skip to the first offset page
    unfs_ds_t* ds = nodep->ds;
    u64 pageoff = offset >> UNFS_PAGESHIFT;
    while (pageoff >= ds->pagecount) {
        pageoff -= ds->pagecount;
        ds++;
    }
    u64 dspc = ds->pagecount - pageoff;
    u64 pa = ds->pageid + pageoff;
    u64 byteoff = offset & (UNFS_PAGESIZE - 1);
    u64 pagecount = PAGECOUNT(byteoff + len);
    u64 endlen = (byteoff + len) & (UNFS_PAGESIZE - 1);

    // perform read write
    u32 iopc = (len << UNFS_PAGESHIFT) + 1;
    void* iop = unfs.dev.page_alloc(ioc, &iopc);
    for (;;) {
        u64 pc = pagecount;
        if (pc > dspc) pc = dspc;
        if (pc > iopc) pc = iopc;
        u64 iolen = (pc << UNFS_PAGESHIFT) - byteoff;
        if (iolen > len) iolen = len;
        if (wflag) {
            void* miop = iop;
            void* mbuf = buf;
            u64 mlen = iolen;

            // check for offset in the first page
            if (byteoff) {
                unfs.dev.read(ioc, iop, pa, 1);
                u64 n = UNFS_PAGESIZE - byteoff;
                if (endlen && pagecount == 1) {
                    n = len;
                    endlen = 0;
                }
                memcpy(iop + byteoff, buf, n);
                miop += UNFS_PAGESIZE;
                mbuf += n;
                mlen -= n;
            }

            // check for partial length in the last page
            if (endlen && pc == pagecount) {
                u64 n = (pc - 1) << UNFS_PAGESHIFT;
                unfs.dev.read(ioc, iop + n, pa + pc - 1, 1);
                memcpy(iop + n, buf + len - endlen, endlen);
                mlen -= endlen;
            }

            if (mlen) memcpy(miop, mbuf, mlen);
            unfs.dev.write(ioc, iop, pa, pc);
        } else {
            // read and copy out
            unfs.dev.read(ioc, iop, pa, pc);
            memcpy(buf, iop + byteoff, iolen);
        }

        len -= iolen;
        if (len == 0) break;
        pagecount -= pc;
        byteoff = 0;
        buf += iolen;
        if (pc < dspc) {
            pa += pc;
            dspc -= pc;
        } else {
            ds++;
            pa = ds->pageid;
            dspc = ds->pagecount;
        }
    }
    unfs.dev.page_free(ioc, iop, iopc);

    return 0;
}

/**
 * Merge all data segments of a file into a newly allocated one.
 * This needs to be done when number of file segments reached its max limit.
 * @param   ioc         io context
 * @param   nodep       file pointer
 * @param   newsize     file new size in byte count
 * @return  0 if ok else error code.
 */
static int unfs_node_merge_ds(unfs_ioc_t ioc, unfs_node_t* nodep, u64 newsize)
{
    DEBUG_FN("merge %s dsc=%u size=%#lx", nodep->name, nodep->dscount, newsize);
    u64 pagecount = PAGECOUNT(newsize);

    u64 pageid = unfs_map_alloc(pagecount);
    if (pageid == 0) return ENOSPC;
    u64 pa = pageid;
    u32 iopc = (newsize << UNFS_PAGESHIFT) + 1;
    void* iop = unfs.dev.page_alloc(ioc, &iopc);
    int i;
    for (i = 0; i < nodep->dscount; i++) {
        unfs_ds_t* ds = &nodep->ds[i];
        u64 dspa = ds->pageid;
        u64 dspc = ds->pagecount;
        while (dspc) {
            u64 pc = dspc;
            if (pc > iopc) pc = iopc;
            unfs.dev.read(ioc, iop, dspa, pc);
            unfs.dev.write(ioc, iop, pa, pc);
            pa += pc;
            dspa += pc;
            dspc -= pc;
        }
        unfs_map_free(ds->pageid, ds->pagecount);
        ds->pageid = 0;
        ds->pagecount = 0;
    }
    unfs.dev.page_free(ioc, iop, iopc);

    nodep->ds[0].pageid = pageid;
    nodep->ds[0].pagecount = pagecount;
    nodep->dscount = 1;
    nodep->updated = 1;
    return 0;
}

/**
 * Resize a file node.
 * @param   ioc         io context
 * @param   nodep       file pointer
 * @param   newsize     file new size in byte count
 * @param   fill        pointer to a pattern to fill the newly added portion
 * @return  0 if ok else error code.
 */
static int unfs_node_resize(unfs_ioc_t ioc, unfs_node_t* nodep, u64 newsize, int* fill)
{
    u64 oldsize = nodep->size;
    DEBUG_FN("%s from %#lx to %#lx", nodep->name, oldsize, newsize);
    if (oldsize == newsize) return 0;

    // size increase may require adding segments
    if (newsize > oldsize) {
        // zero filled the uninit portion of the last page of the last segment
        u64 zlen =  oldsize & (UNFS_PAGESIZE - 1);
        if (fill && zlen) {
            u32 iopc = 1;
            void* iop = unfs.dev.page_alloc(ioc, &iopc);
            if (iopc != 1)
                FATAL("cannot allocate 1 page");
            unfs_ds_t* ds = &nodep->ds[nodep->dscount - 1];
            u64 pa = ds->pageid + ds->pagecount - 1;
            unfs.dev.read(ioc, iop, pa, 1);
            memset(iop + zlen, *fill, UNFS_PAGESIZE - zlen);
            unfs.dev.write(ioc, iop, pa, 1);
            unfs.dev.page_free(ioc, iop, iopc);
        }

        // check to add new segment for additional page(s)
        u64 addpc = PAGECOUNT(newsize) - PAGECOUNT(oldsize);
        if (addpc > 0) {
            u64 pageid = 0;
            if (nodep->dscount < UNFS_MAXDS) {
                // if segment is available then check to add one
                pageid = unfs_map_alloc(addpc);
                if (pageid == 0) return ENOSPC;
                int i = nodep->dscount - 1;
                unfs_ds_t* dsp = nodep->ds + i;
                // if contiguous then modify last segment else add new segment
                if (i >= 0 && pageid == (dsp->pageid + dsp->pagecount)) {
                    dsp->pagecount += addpc;
                    DEBUG_FN("%s mod ds[%d]=(%#lx %#lx) page=%#lx",
                          nodep->name, i, dsp->pageid, dsp->pagecount, pageid);
                } else {
                    i++;
                    dsp++;
                    nodep->dscount++;
                    dsp->pageid = pageid;
                    dsp->pagecount = addpc;
                    DEBUG_FN("%s add ds[%d]=(%#lx %#lx)",
                                    nodep->name, i, pageid, addpc);
                }
            } else {
                // if no segment is available then merge all into one
                int err = unfs_node_merge_ds(ioc, nodep, newsize);
                if (err) return err;
                pageid = nodep->ds[0].pageid + PAGECOUNT(oldsize);
            }

            if (fill) {
                u64 pc = addpc;
                u32 iopc = addpc;
                void* iop = unfs.dev.page_alloc(ioc, &iopc);
                if (pc > iopc) pc = iopc;
                memset(iop, *fill, pc << UNFS_PAGESHIFT);

                while (addpc) {
                    if (pc > addpc) pc = addpc;
                    unfs.dev.write(ioc, iop, pageid, pc);
                    pageid += pc;
                    addpc -= pc;
                }
                unfs.dev.page_free(ioc, iop, iopc);
            }
        }

    // size decrease may require deleting segments
    } else {
        u64 delpc = PAGECOUNT(oldsize) - PAGECOUNT(newsize);
        unfs_ds_t* ds = &nodep->ds[nodep->dscount-1];
        while (delpc) {
            if (ds->pagecount > delpc) {
                ds->pagecount -= delpc;
                unfs_map_free(ds->pageid + ds->pagecount, delpc);
                break;
            }
            unfs_map_free(ds->pageid, ds->pagecount);
            delpc -= ds->pagecount;
            nodep->dscount--;
            ds--;
        }
    }

    nodep->size = newsize;
    nodep->updated = 1;
    return 0;
}

/**
 * Create a file/directory.
 * @param   name        canonical name
 * @param   isdir       directory flag
 * @return  node pointer or NULL if error.
 */
static unfs_node_t* unfs_node_create(const char *name, int isdir)
{
    DEBUG_FN("%s", name);
    unfs_node_t* parent = unfs_node_find_parent(name);
    if (!parent) {
        ERROR("Parent directory of %s does not exist", name);
        return NULL;
    }

    unfs_node_t node;
    node.pageid = unfs_node_alloc(isdir);
    if (node.pageid == 0) return NULL;
    node.name = (char*)name;
    node.parent = parent;
    node.parentid = parent->pageid;
    node.size = 0;
    node.isdir = isdir;
    node.open = 0;
    node.dscount = 0;

    unfs_node_t* newnodep = unfs_node_add(parent, &node);
    parent->size++;
    unfs_ioc_t ioc = unfs.dev.ioc_alloc();
    unfs_node_sync(ioc, parent);
    unfs_node_sync(ioc, newnodep);
    unfs.dev.ioc_free(ioc);
    return newnodep;
}

/**
 * Open/Create a file.
 * @param   fs          filesystem reference
 * @param   name        canonical name
 * @param   mode        open mode
 * @return  file descriptor reference or NULL if error.
 */
unfs_fd_t unfs_file_open(unfs_fs_t fs, const char *name, unfs_mode_t mode)
{
    DEBUG_FN("%s", name);
    unfs_fd_t fd = { .error = 0, .mode = mode, .id = NULL };

    if (FS_CHECK(fs) || strlen(name) >= UNFS_MAXPATH) {
        fd.error = EINVAL;
        return fd;
    }

    FS_WRLOCK();
    unfs_node_t* nodep = unfs_node_find(name);
    if (nodep) {
        FILE_WRLOCK(nodep);
        if ((mode & UNFS_OPEN_EXCLUSIVE) && nodep->open) {
            FILE_UNLOCK(nodep);
            fd.error = EBUSY;
            goto done;
        }
        nodep->open++;
        FILE_UNLOCK(nodep);
    } else {
        if (!(mode & UNFS_OPEN_CREATE)) {
            fd.error = ENOENT;
            goto done;
        }
        nodep = unfs_node_create(name, 0);
        nodep->open++;
    }
    fd.id = nodep;

done:
    FS_UNLOCK();
    return fd;
}

/**
 * Close a file.
 * @param   fd          file descriptor reference
 * @return  0 if ok else error code.
 */
int unfs_file_close(unfs_fd_t fd)
{
    int err = EINVAL;
    unfs_node_t* nodep = fd.id;

    FILE_WRLOCK(nodep);
    DEBUG_FN("%s %d", nodep->name, nodep->open);
    if (nodep->open) {
        nodep->open--;
        if (nodep->updated) {
            unfs_ioc_t ioc = unfs.dev.ioc_alloc();
            unfs_node_sync(ioc, nodep);
            unfs.dev.write(ioc, unfs.header, UNFS_HEADPA, UNFS_HEADPC);
            unfs.dev.ioc_free(ioc);
            nodep->updated = 0;
        }
        err = 0;
    }
    FILE_UNLOCK(nodep);
    return err;
}

/**
 * Sync file state to device.
 * @param   fd          file descriptor reference
 * @return  0 if ok else error code.
 */
int unfs_file_sync(unfs_fd_t fd)
{
    int err = EINVAL;
    unfs_node_t* nodep = fd.id;

    FILE_WRLOCK(nodep);
    DEBUG_FN("%s", nodep->name);
    if (nodep->open) {
        if (nodep->updated) {
            unfs_ioc_t ioc = unfs.dev.ioc_alloc();
            unfs_node_sync(ioc, nodep);
            unfs.dev.write(ioc, unfs.header, UNFS_HEADPA, UNFS_HEADPC);
            unfs.dev.ioc_free(ioc);
            nodep->updated = 0;
        }
        err = 0;
    }
    FILE_UNLOCK(nodep);
    return err;
}

/**
 * Return the file name.
 * @param   fd          file descriptor reference
 * @param   name        returned name buffer
 * @param   len         name buffer length
 * @return  the name buffer if specified or allocated string or NULL if error.
 */
char* unfs_file_name(unfs_fd_t fd, char* name, int len)
{
    unfs_node_t* nodep = fd.id;

    FILE_RDLOCK(nodep);
    char* s = NULL;
    if (nodep->open) {
        s = name ? strncpy(name, nodep->name, len) : strdup(nodep->name);
    }
    FILE_UNLOCK(nodep);
    return s;
}

/**
 * Return the file status including size and data segment info.
 * If dslp is not NULL, a ds segment array will be allocated and
 * caller must free it when done.
 * @param   fd          file descriptor reference
 * @param   sizep       file size pointer
 * @param   dscp        number of ds entry count pointer
 * @param   dslp        ds entry list pointer
 * @return  an allocated array of file data segments.
 */
int unfs_file_stat(unfs_fd_t fd, u64* sizep, u32* dscp, unfs_ds_t** dslp)
{
    int err = EINVAL;
    unfs_node_t* nodep = fd.id;

    FILE_RDLOCK(nodep);
    DEBUG_FN("%s", nodep->name);
    if (nodep->open) {
        if (sizep) *sizep = nodep->size;
        if (dscp) *dscp = nodep->dscount;
        if (dslp) {
            *dslp = NULL;
            if (nodep->dscount) {
                size_t n = nodep->dscount * sizeof(unfs_ds_t);
                *dslp = malloc(n);
                memcpy(*dslp, nodep->ds, n);
            }
        }
        err = 0;
    }
    FILE_UNLOCK(nodep);
    return err;
}

/**
 * Resize a file.
 * @param   fd          file descriptor reference
 * @param   newsize     new size
 * @param   fill        pointer to a pattern to fill the newly added portion
 * @return  0 if ok else error code.
 */
int unfs_file_resize(unfs_fd_t fd, u64 newsize, int* fill)
{
    int err = EINVAL;
    unfs_node_t* nodep = fd.id;

    FILE_WRLOCK(nodep);
    DEBUG_FN("%s %#lx", nodep->name, newsize);
    if (nodep->open) {
        FS_WRLOCK();
        unfs_ioc_t ioc = unfs.dev.ioc_alloc();
        unfs_node_resize(ioc, nodep, newsize, fill);
        unfs.dev.ioc_free(ioc);
        FS_UNLOCK();
        err = 0;
    }
    FILE_UNLOCK(nodep);
    return err;
}

/**
 * Read data from a file into a user buffer.
 * @param   fd          file descriptor reference
 * @param   buf         data buffer
 * @param   offset      file offset position
 * @param   len         number of bytes to read
 * @return  0 if ok else error code.
 */
int unfs_file_read(unfs_fd_t fd, void *buf, u64 offset, u64 len)
{
    int err = EINVAL;
    unfs_node_t* nodep = fd.id;

    FILE_RDLOCK(nodep);
    DEBUG_FN("%s off=%#lx len=%#lx size=%#lx", nodep->name, offset, len, nodep->size);
    if (nodep->open) {
        if ((offset + len) <= nodep->size) {
            unfs_ioc_t ioc = unfs.dev.ioc_alloc();
            err = unfs_node_rw(ioc, nodep, buf, offset, len, 0);
            unfs.dev.ioc_free(ioc);
        } else {
            ERROR("%s off=%#lx len=%#lx size=%#lx", nodep->name, offset, len, nodep->size);
        }
    }
    FILE_UNLOCK(nodep);
    return err;
}

/**
 * Write data into a file.
 * @param   fd          file descriptor reference
 * @param   buf         data buffer
 * @param   offset      file offset position
 * @param   len         number of bytes to read
 * @return  0 if ok else error code.
 */
int unfs_file_write(unfs_fd_t fd, const void *buf, u64 offset, u64 len)
{
    int err = EINVAL;
    unfs_node_t* nodep = fd.id;

    DEBUG_FN("%s off=%#lx len=%#lx size=%#lx", nodep->name, offset, len, nodep->size);
#if 0
    // file offset should not exceed file size
    if (offset > nodep->size) {
        ERROR("%s off=%#lx len=%#lx size=%#lx", nodep->name, offset, len, nodep->size);
        return err;
    }
#endif

    FILE_WRLOCK(nodep);
    if (nodep->open) {
        err = 0;
        u64 size = offset + len;
        unfs_ioc_t ioc;
        if (size > nodep->size) {
            FS_WRLOCK();
            ioc = unfs.dev.ioc_alloc();
            err = unfs_node_resize(ioc, nodep, size, NULL);
            FS_UNLOCK();
        } else {
            ioc = unfs.dev.ioc_alloc();
        }
        if (!err) err = unfs_node_rw(ioc, nodep, (void*)buf, offset, len, 1);
        unfs.dev.ioc_free(ioc);
    }
    FILE_UNLOCK(nodep);
    return err;
}

/**
 * Calculate a 64-bit file checksum.  Checksum may not be content unique.
 * @param   fd          file descriptor reference
 * @return  the checksum or -1 if error.
 */
u64 unfs_file_checksum(unfs_fd_t fd)
{
    u64 sum = 0;
    unfs_node_t* nodep = fd.id;

    FILE_RDLOCK(nodep);
    DEBUG_FN("%s", nodep->name);
    if (nodep->open) {
        int d, p, i;
        u32 iopc = 1;
        unfs_ioc_t ioc = unfs.dev.ioc_alloc();
        void* iop = unfs.dev.page_alloc(ioc, &iopc);
        if (iopc != 1)
            FATAL("cannot allocate 1 page");
        u64 size = nodep->size;
        for (d = 0; d < nodep->dscount; d++) {
            u64 pa = nodep->ds[d].pageid;
            for (p = 0; p < nodep->ds[d].pagecount; p++) {
                unfs.dev.read(ioc, iop, pa, 1);
                u8* bp = iop;
                for (i = 0; i < UNFS_PAGESIZE; i++) {
                    sum += (size << 32) | *bp++;
                    if (--size == 0) break;
                }
                pa++;
            }
        }
        unfs.dev.page_free(ioc, iop, iopc);
        unfs.dev.ioc_free(ioc);
    }
    FILE_UNLOCK(nodep);
    return sum;
}

/**
 * Check if name matched and add to directory listing.
 * @param   nodep       node pointer
 * @param   dlp         directory list pointer
 */
static void unfs_dir_match(unfs_node_t* nodep, unfs_dir_list_t* dlp)
{
    if (unfs_child_of(nodep->name, dlp->name)) {
        if (dlp->size == 0)
            FATAL("bad directory size");
        int n = --dlp->size;
        dlp->list[n].name = strdup(nodep->name);
        dlp->list[n].size = nodep->size;
        dlp->list[n].isdir = nodep->isdir;
    }
}

/**
 * Walk the tree to get a directory listing.
 * @param   root        root node
 * @param   dlp         directory list pointer
 */
static void unfs_dir_walk(struct tnode* root, unfs_dir_list_t* dlp)
{
    if (!root) return;

    if (root->left == NULL && root->right == NULL) {
        unfs_dir_match((unfs_node_t*)(root->key), dlp);
    } else {
        if (root->left != NULL) unfs_dir_walk(root->left, dlp);
        unfs_dir_match((unfs_node_t*)(root->key), dlp);
        if (root->right != NULL) unfs_dir_walk(root->right, dlp);
    }
}

/**
 * Get a directory listing.
 * @param   fs          filesystem reference
 * @param   name        canonical name
 * @return  an allocated directory list structure.
 */
unfs_dir_list_t* unfs_dir_list(unfs_fs_t fs, const char *name)
{
    unfs_dir_list_t* dlp = NULL;
    DEBUG_FN("%s", name);
    if (FS_CHECK(fs) || strlen(name) >= UNFS_MAXPATH) return dlp;

    FS_RDLOCK();
    unfs_node_t* nodep = unfs_node_find(name);
    if (nodep && nodep->isdir) {
        u64 nodesize = nodep->size;
        dlp = malloc(sizeof(*dlp) + (nodesize * sizeof(unfs_dir_entry_t)));
        dlp->name = strdup(nodep->name);
        dlp->size = nodesize;
        unfs_dir_walk(unfs.root, dlp);
        if (dlp->size != 0)
            FATAL("size=%#lx found=%#lx", nodesize, nodesize - dlp->size);
        dlp->size = nodesize;
    }
    FS_UNLOCK();
    return dlp;
}

/**
 * Free a directory_list structure.
 * @param   dlp         directory list pointer
 */
void unfs_dir_list_free(unfs_dir_list_t* dlp)
{
    DEBUG_FN("%s", dlp->name);
    int count = dlp->size;
    while (count) free(dlp->list[--count].name);
    free(dlp->name);
    free(dlp);
}

/**
 * Create a file or directory if it doesn't exist.
 * If pflag is set, then create the parent directories as needed.
 * @param   fs          filesystem reference
 * @param   name        canonical name
 * @param   isdir       is directory flag
 * @param   pflag       make parent directories as needed
 * @return  0 if ok, else error code.
 */
int unfs_create(unfs_fs_t fs, const char *name, int isdir, int pflag)
{
    DEBUG_FN("%s", name);
    if (FS_CHECK(fs) || strlen(name) >= UNFS_MAXPATH) return EINVAL;

    int err = 0;
    FS_WRLOCK();
    if (pflag) {
        char path[UNFS_MAXPATH];
        strcpy(path, name);
        int dir = 1;
        char* s = path + 1;
        while (s && *s != '\0') {
            if ((s = strchr(s, '/'))) *s = 0;
            else dir = isdir;
            unfs_node_t* nodep = unfs_node_find(path);
            if (!nodep) nodep = unfs_node_create(path, dir);
            if (!nodep) {
                err = ENOMEM;
                break;
            }
            if (s) *s++ = '/';
        }
    } else {
        unfs_node_t* nodep = unfs_node_find(name);
        if (!nodep) {
            nodep = unfs_node_create(name, isdir);
            if (!nodep) err = ENOMEM;
        }
    }
    FS_UNLOCK();
    return err;
}

/**
 * Remove a file or directory.  If node is directory, it must be empty.
 * @param   fs          filesystem reference
 * @param   name        canonical name
 * @param   isdir       is directory flag
 * @return  0 if ok else error code.
 */
int unfs_remove(unfs_fs_t fs, const char *name, int isdir)
{
    DEBUG_FN("%s", name);
    if (FS_CHECK(fs) || name[1] == 0 || strlen(name) >= UNFS_MAXPATH)
        return EINVAL;

    int err = 0;
    FS_WRLOCK();
    unfs_node_t* nodep = unfs_node_find(name);
    if (!nodep || nodep->isdir != isdir) {
        err = ENOENT;
    } else if (nodep->open || (isdir && nodep->size != 0)) {
        err = EBUSY;
    } else {
        unfs_ioc_t ioc = unfs.dev.ioc_alloc();
        unfs_node_remove(ioc, nodep);
        unfs.dev.ioc_free(ioc);
    }
    FS_UNLOCK();
    return err;
}

/**
 * Rename/Move a directory or file.  If node is directory, it must be empty.
 * @param   fs          filesystem reference
 * @param   src         source canonical name
 * @param   dst         destination canonical name
 * @param   override    delete to if to exists
 * @return  0 if ok else error code.
 */
int unfs_rename(unfs_fs_t fs, const char *src, const char *dst, int override)
{
    DEBUG_FN("%s to %s", src, dst);
    if (FS_CHECK(fs) || src[1] == 0 ||
        strlen(src) >= UNFS_MAXPATH || strlen(dst) >= UNFS_MAXPATH)
        return EINVAL;

    int err = 0;
    FS_WRLOCK();
    unfs_ioc_t ioc = unfs.dev.ioc_alloc();

    // src-node must exist
    unfs_node_t* srcnode = unfs_node_find(src);
    if (!srcnode) {
        err = ENOENT;
        goto done;
    }
    unfs_node_t* srcparent = srcnode->parent;

    // src-node must not be opened and if isdir then must be empty
    if (srcnode->open || (srcnode->isdir && srcnode->size)) {
        err = EBUSY;
        goto done;
    }

    // dst-parent must exist
    unfs_node_t* dstparent = unfs_node_find_parent(dst);
    if (!dstparent) {
        err = EINVAL;
        goto done;
    }

    // if override flag set then dst-node must not exist of will be deleted
    unfs_node_t* dstnode = unfs_node_find(dst);
    if (dstnode) {
        if (override) {
            if (dstnode->open || (dstnode->isdir && dstnode->size != 0)) {
                err = EBUSY;
                goto done;
            }
            unfs_node_remove(ioc, dstnode);
        } else {
            err = EEXIST;
            goto done;
        }
    }

    // remove the node, change its name, and put back in tree
    tdelete(srcnode, &unfs.root, unfs_node_cmp_fn);
    int namelen = strlen(dst);
    size_t nsize = srcnode->isdir ? sizeof(unfs_node_t) : UNFS_PAGESIZE;
    size_t memsize = nsize + namelen + 1;
    if (srcnode->memsize < memsize) srcnode = realloc(srcnode, memsize);
    srcnode->memsize = memsize;
    srcnode->name = (char*)srcnode + nsize;
    strcpy(srcnode->name, dst);
    srcnode->parent = dstparent;
    srcnode->parentid = dstparent->pageid;
    tsearch(srcnode, &unfs.root, unfs_node_cmp_fn);

    // sync node and parents
    unfs_node_sync(ioc, srcnode);
    if (srcparent != dstparent) {
        FILE_WRLOCK(srcparent);
        srcparent->size--;
        unfs_node_sync(ioc, srcparent);
        FILE_UNLOCK(srcparent);

        FILE_WRLOCK(dstparent);
        dstparent->size++;
        unfs_node_sync(ioc, dstparent);
        FILE_UNLOCK(dstparent);
    }

done:
    unfs.dev.ioc_free(ioc);
    FS_UNLOCK();
    return err;
}

/**
 * Check if a node name exists and return its size.
 * @param   fs          filesystem reference
 * @param   name        canonical name
 * @param   isdir       is directory flag pointer
 * @param   sizep       size pointer if node exist
 * @return  1 if exist else 0.
 */
int unfs_exist(unfs_fs_t fs, const char *name, int* isdir, u64* sizep)
{
    DEBUG_FN("%s", name);
    int exist = 0;
    if (FS_CHECK(fs)) return exist;

    FS_RDLOCK();
    unfs_node_t* nodep = unfs_node_find(name);
    if (nodep) {
        if (isdir) *isdir = nodep->isdir;
        if (sizep) *sizep = nodep->size;
        exist = 1;
    }
    FS_UNLOCK();
    return exist;
}

/**
 * Get a copy of the UNFS header info.
 * @param   fs          filesystem reference
 * @param   statp       header statistics pointer
 * @param   print       print header flag
 * @return  0 if ok else error code.
 */
int unfs_stat(unfs_fs_t fs, unfs_header_t* statp, int print)
{
    DEBUG_FN();
    if (FS_CHECK(fs)) return EINVAL;

    FS_RDLOCK();
    memcpy(statp, unfs.header, sizeof(*unfs.header));
    FS_UNLOCK();

    if (print) unfs_print_header(statp);
    return 0;
}

/**
 * Open the appropriate driver implementation based on the given device name.
 * @param   device      device name
 * @return  allocated header or NULL if device name is not supported.
 */
static unfs_header_t* unfs_open_dev(const char* device)
{
    if (unfs.header) {
        if (strcmp(unfs.dev.name, device) != 0)
            FATAL("device %s is in use", unfs.dev.name);
    } else {
        int n;
        if (sscanf(device, "%x:%x.%x", &n, &n, &n) == 3) {
            unfs_header_t* unfs_unvme_open(unfs_device_io_t*, const char*);
            unfs.header = unfs_unvme_open(&unfs.dev, device);
        } else if (strncmp(device, "/dev/", 5) == 0) {
            unfs_header_t* unfs_raw_open(unfs_device_io_t*, const char*);
            unfs.header = unfs_raw_open(&unfs.dev, device);
        } else {
            FATAL("unknown device %s", device);
        }
        unfs.dev.name = strdup(device);
    }
    return unfs.header;
}

/**
 * Initialize and open the device.
 * @param   device      device name
 */
static void unfs_init(const char* device)
{
    pthread_mutex_lock(&unfslock);
    LOG_OPEN();
    INFO_FN("%s", device);
    if (!unfs.header) {
        pthread_rwlock_init(&unfs.lock, NULL);
        unfs.header = unfs_open_dev(device);
        unfs.header->pagefree = unfs.header->pagecount;
        unfs.fsid = time(0) << 16;
        unfs.mapsynclo = unfs.header->pagecount;
        unfs.mapsynchi = 0;
        unfs.mapsyncfdlo = unfs.header->pagecount;
        unfs.mapsyncfdhi = 0;
    }
    pthread_mutex_unlock(&unfslock);
}

/**
 * Sync filesystem header and map to disk.
 */
static void unfs_sync()
{
    if (unfs.mapsynclo <= unfs.mapsynchi ||
        unfs.mapsyncfdlo <= unfs.mapsyncfdhi) {
        unfs_ioc_t ioc = unfs.dev.ioc_alloc();

        unfs.dev.write(ioc, unfs.header, UNFS_HEADPA, UNFS_HEADPC);
        if (unfs.mapsynclo <= unfs.mapsynchi) {
            u64 pa = (unfs.mapsynclo - unfs.header->datapage) >> 15;
            u32 pc = ((unfs.mapsynchi - unfs.header->datapage) >> 15) - pa + 1;
            unfs.dev.write(ioc, unfs.header->map + pa, UNFS_MAPPA + pa, pc); 
            unfs.mapsynclo = unfs.header->pagecount;
            unfs.mapsynchi = 0;
        }
        if (unfs.mapsyncfdlo <= unfs.mapsyncfdhi) {
            u64 pa = (unfs.mapsyncfdlo - unfs.header->datapage) >> 15;
            u32 pc = ((unfs.mapsyncfdhi - unfs.header->datapage) >> 15) - pa + 1;
            unfs.dev.write(ioc, unfs.header->map + pa, UNFS_MAPPA + pa, pc); 
            unfs.mapsyncfdlo = unfs.header->pagecount;
            unfs.mapsyncfdhi = 0;
        }
        unfs.dev.ioc_free(ioc);
    }
}

/**
 * Cleanup and close the UNVMe based UNFS filesystem.
 */
void unfs_cleanup()
{
    INFO_FN();
    pthread_mutex_trylock(&unfslock);
    unfs_sync();
    if (unfs.header) {
        FS_TRYLOCK();
        tdestroy(unfs.root, free);
        unfs.dev.close();
        free(unfs.dev.name);
        FS_UNLOCK();
        pthread_rwlock_destroy(&unfs.lock);
    }
    memset(&unfs, 0, sizeof(unfs));
    LOG_CLOSE();
    pthread_mutex_unlock(&unfslock);
}

/**
 * Close a filesystem access.
 * @param   fs          filesystem reference
 */
int unfs_close(unfs_fs_t fs)
{
    DEBUG_FN();
    if (FS_CHECK(fs)) return EINVAL;

    // sync header and bitmap
    FS_WRLOCK();
    unfs_sync();
    FS_UNLOCK();

    if (__sync_fetch_and_sub(&unfs.open, 1) <= 1) unfs_cleanup();
    return 0;
}

/**
 * Open to access the filesystem.
 * @param   device      device name
 * @return  a filesystem handle or 0 upon failure.
 */
unfs_fs_t unfs_open(const char* device)
{
    unfs_init(device);
    u64 i = __sync_add_and_fetch(&unfs.open, 1);
    unfs_fs_t fs = __sync_add_and_fetch(&unfs.fsid, 1);
    DEBUG_FN("%s %#lx %d", device, fs, i);

    FS_WRLOCK();
    unfs_header_t* hp = unfs.header;
    u64 pagecount = hp->pagecount;
    u64 datapage = hp->datapage;
    u64 mapsize = (pagecount - datapage + 63) >> 6;   // in 64-bit words

    // allocate IO pages
    unfs_ioc_t ioc = unfs.dev.ioc_alloc();
    u32 iopc = UNFS_FILEPC;
    unfs_node_io_t* niop = unfs.dev.page_alloc(ioc, &iopc);
    if (iopc != UNFS_FILEPC)
        FATAL("cannot allocate %u pages", UNFS_FILEPC);

    // read and validate the UNFS header
    unfs.dev.read(ioc, hp, UNFS_HEADPA, datapage);
    u64 pagefree = pagecount - unfs_map_count();
    DEBUG_FN("pc=%#lx pf=%#lx dp=%#lx ms=%#lx fp=%#lx fc=%#lx dc=%u",
                                pagecount, pagefree, datapage, mapsize,
                                hp->fdnextpage, hp->fdcount, hp->delcount);
    if (strcmp(hp->version, UNFS_VERSION) ||
        (hp->pagecount != pagecount) ||
        (hp->datapage != datapage) ||
        (hp->mapsize != mapsize) ||
        (hp->pagefree != pagefree) ||
        ((hp->fdnextpage + ((hp->fdcount + hp->delcount + 1) * UNFS_FILEPC)) != pagecount)) {
        ERROR("bad UNFS header (pf=%#lx)", pagefree);
        unfs_print_header(hp);
        fs = 0L;
        goto done;
    }

    // next bitmap free index
    u64* map = (u64*)hp->map;
    for (i = 0; (i < unfs.header->mapsize) && (*map == -1L); i++) map++;
    unfs.mapnext = i;

    // read each file entry and build the node tree in memory
    u64 pa = pagecount - UNFS_FILEPC;
    for (i = 0; i < hp->fdcount; pa -= UNFS_FILEPC) {
        // skip over the deleted entries
        int d;
        for (d = 0; d < unfs.header->delcount; d++) {
            if (pa == unfs.header->delstack[d]) {
                DEBUG_FN("skip.%d %#lx", d, pa);
                d = -1;
                break;
            }
        }
        if (d == -1) continue;

        // read entry
        unfs.dev.read(ioc, niop, pa, UNFS_FILEPC);
        DEBUG_FN("scan.%lx %#lx %s", i, pa, niop->name);

        // if node exists then update it, else add new one
        unfs_node_t* nodep = unfs_node_find(niop->name);
        if (nodep) {
            if (nodep->isdir && nodep->pageid == 0) {
                nodep->pageid = niop->node.pageid;
                nodep->size =  niop->node.size;
                nodep->parentid = niop->node.parentid;
                DEBUG_FN("update %s %#lx %#lx", nodep->name, nodep->pageid, nodep->parentid);
            } else {
                FATAL("%s loaded at %#lx seen again at %#lx",
                        nodep->name, nodep->pageid, niop->node.pageid);
            }
        } else {
            unfs_node_t* parent = NULL;
            if (niop->name[1]) parent = unfs_node_add_parents(niop->name);
            niop->node.name = niop->name;
            nodep = unfs_node_add(parent, &niop->node);
        }
        i++;
    }

done:
    unfs.dev.page_free(ioc, niop, iopc);
    unfs.dev.ioc_free(ioc);
    FS_UNLOCK();
    return fs;
}

/**
 * Open the UNFS filesystem and verify every node parent child relationship.
 * @param   device      device name
 * @return  0 if ok else error code.
 */
int unfs_check(const char* device)
{
    unfs_init(device);
    DEBUG_FN("%s", device);
    FS_WRLOCK();
    unfs_header_t* hp = unfs.header;
    u64 pagecount = hp->pagecount;
    u64 datapage = hp->datapage;
    u64 mapsize = (pagecount - datapage + 63) >> 6;   // in 64-bit words
    int err = EINVAL;

    // allocate IO pages
    unfs_ioc_t ioc = unfs.dev.ioc_alloc();
    u32 iopc = 2 * UNFS_FILEPC;
    unfs_node_io_t* niop = unfs.dev.page_alloc(ioc, &iopc);
    if (iopc != 2 * UNFS_FILEPC)
        FATAL("cannot allocate %u pages", 2 * UNFS_FILEPC);
    unfs_node_io_t* piop = niop + 1;

    // read and validate the UNFS header format
    unfs.dev.read(ioc, hp, UNFS_HEADPA, datapage);
    u64 pagefree = pagecount - unfs_map_count();
    DEBUG_FN("pc=%#lx pf=%#lx dp=%#lx ms=%#lx fp=%#lx fc=%#lx dc=%u",
                                pagecount, pagefree, datapage, mapsize,
                                hp->fdnextpage, hp->fdcount, hp->delcount);
    if (strcmp(hp->version, UNFS_VERSION) ||
        (hp->pagecount != pagecount) ||
        (hp->datapage != datapage) ||
        (hp->mapsize != mapsize) ||
        (hp->pagefree != pagefree) ||
        ((hp->fdnextpage + (hp->fdcount + hp->delcount + 1) * UNFS_FILEPC) != pagecount)) {
        ERROR("bad UNFS header (pf=%#lx)", pagefree);
        unfs_print_header(hp);
        goto done;
    }

    // read each file entry and verify their parent and map usage
    u64 pa = pagecount - UNFS_FILEPC;
    u64 i;
    for (i = 0; i < hp->fdcount; pa -= UNFS_FILEPC) {
        // skip over the deleted entries
        int d;
        for (d = 0; d < hp->delcount; d++) {
            if (hp->delstack[d] == pa) {
                DEBUG_FN("skip.%d %#lx", d, pa);
                d = -1;
                break;
            }
        }
        if (d == -1) continue;

        unfs.dev.read(ioc, niop, pa, UNFS_FILEPC);
        DEBUG_FN("scan.%lx %#lx %s", i, pa, niop->name);

        // check map usage
        if (unfs_map_check(niop->node.pageid, UNFS_FILEPC)) {
            ERROR("%s page %#lx bits not set", niop->name, niop->node.pageid);
            goto done;
        }
        unfs_ds_t* dsp = niop->node.ds;
        for (d = 0; d < niop->node.dscount; d++) {
            if (unfs_map_check(dsp->pageid, dsp->pagecount)) {
                ERROR("%s ds[%d]=(%#lx %#lx) bits not set",
                        niop->name, d, dsp->pageid, dsp->pagecount);
                goto done;
            }
        }

        // check parent node
        if (niop->name[1]) {
            u64 parentid = niop->node.parentid;
            if (parentid <= hp->fdnextpage || parentid >= pagecount) {
                ERROR("%s has bad parentid %#lx", niop->name, parentid);
                goto done;
            }
            unfs.dev.read(ioc, piop, parentid, UNFS_FILEPC);
            if (!unfs_child_of(niop->name, piop->name)) {
                ERROR("%s is not a child of %s", niop->name, piop->name);
                goto done;
            }
        }
        i++;
    }
    err = 0;

done:
    unfs.dev.page_free(ioc, niop, iopc);
    unfs.dev.ioc_free(ioc);
    FS_UNLOCK();
    unfs_cleanup();
    return err;
}

/**
 * Create a new UNFS filesystem.
 * @param   device      device name
 * @param   label       disk label
 * @param   print       print header flag
 * @return  0 if ok else error code.
 */
int unfs_format(const char* device, const char* label, int print)
{
    unfs_init(device);
    DEBUG_FN("%s", device);

    FS_WRLOCK();
    unfs_header_t* hp = unfs.header;
    strncpy(hp->label, label, sizeof(hp->label) - 1);
    strncpy(hp->version, UNFS_VERSION, sizeof(hp->version) - 1);
    hp->fdnextpage = hp->pagecount - UNFS_FILEPC;
    hp->fdcount = 0;
    hp->dircount = 0;
    hp->mapsize = (hp->pagecount - hp->datapage + 63) >> 6; // in 64-bit words
    hp->delmax = (sizeof(unfs_header_t) - offsetof(unfs_header_t, delstack))
                 / sizeof(u64);

    // create root directory
    unfs_ioc_t ioc = unfs.dev.ioc_alloc();
    u32 iopc = UNFS_FILEPC;
    unfs_node_io_t* niop = unfs.dev.page_alloc(ioc, &iopc);
    if (iopc != UNFS_FILEPC)
        FATAL("cannot allocate %u pages", UNFS_FILEPC);
    memset(niop, 0, sizeof(*niop));
    strcpy(niop->name, "/");
    niop->node.isdir = 1;
    niop->node.pageid = unfs_node_alloc(niop->node.isdir);
    unfs.dev.write(ioc, niop, niop->node.pageid, UNFS_FILEPC);
    unfs.dev.write(ioc, hp, UNFS_HEADPA, hp->datapage);

    if (print) unfs_print_header(hp);

    // free and close
    unfs.dev.page_free(ioc, niop, iopc);
    unfs.dev.ioc_free(ioc);
    FS_UNLOCK();
    unfs_cleanup();
    return 0;
}
