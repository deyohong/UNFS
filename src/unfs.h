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
 * @brief UNFS filesystem header file.
 *
 * The UNFS filesystem disk layout (in 4k-page unit):
 *  Page 0-1:   Filesystem header info with the deleted stack.
 *  Page 2-N:   Bitmap of free pages.
 *  Page N+:    File data starts after free bitmap pages growing upward.
 *  Page Z-:    File/Directory entries start from end of the disk growing
 *              downward.  Each entry will take 2 disk pages.  The first
 *              directory entry (i.e. in the last 2 disk pages) will be
 *              the root directory.
 *
 * Design Notes:
 * -------------
 *  + The bitmap array will track pages starting from the first data page
 *    (i.e. following the bitmap pages) to the last disk page.
 *
 *  + Each file/directory entry will take up 2 disk pages.
 *    The first page contains the file description.
 *    The second page contains the file canonical name.
 *    File and directory share the same structure with a distinguished flag.
 *    Only file has allocated data pages arranged as data segments.
 *
 *  + When a file or directory is added, it will take a page address from
 *    the deleted stack if there is one; otherwise, it will take the next
 *    lowest file page address slot on disk.
 *
 *  + When a file or directory is removed, its page address will be pushed
 *    onto the deleted stack.  When a file is removed, all its data will be
 *    deallocated and put back in the free bit map.
 *
 *  + When a file is written, it will first be resized with new data segment
 *    and new data pages allocation as needed.  When the number of data
 *    segments in a file entry reached its limit, all its segments will
 *    be merged into one.
 *
 *  + All node names must be fully canonical.  Node name can contain any
 *    printable character except '/'.
 *
 *  + No security, permission or timestamp is supported.
 */

#ifndef	_UNFS_H
#define	_UNFS_H

#include <stdint.h>

#ifndef _U_TYPE
#define _U_TYPE                     ///< bit size data types
typedef int8_t          s8;         ///< 8-bit signed
typedef int16_t         s16;        ///< 16-bit signed
typedef int32_t         s32;        ///< 32-bit signed
typedef int64_t         s64;        ///< 64-bit signed
typedef uint8_t         u8;         ///< 8-bit unsigned
typedef uint16_t        u16;        ///< 16-bit unsigned
typedef uint32_t        u32;        ///< 32-bit unsigned
typedef uint64_t        u64;        ///< 64-bit unsigned
#endif // _U_TYPE

#define UNFS_VERSION    "UNFS-1.0"          ///< filesystem version name
#define UNFS_HEADPA     0                   ///< header page address
#define UNFS_HEADPC     2                   ///< header page count
#define UNFS_MAPPA      UNFS_HEADPC         ///< start bitmap page address
#define UNFS_PAGESHIFT  12                  ///< page shift value
#define UNFS_PAGESIZE   (1<<UNFS_PAGESHIFT) ///< expected page size
#define UNFS_MAXPATH    (UNFS_PAGESIZE-2)   ///< max file name length

/// File entry on disk page count
#define UNFS_FILEPC     2

/// Max number of data segments in a file
#define UNFS_MAXDS      ((UNFS_PAGESIZE-sizeof(unfs_node_t))/sizeof(unfs_ds_t))

/// Page size
typedef char unfs_page_t[UNFS_PAGESIZE];

/// File open mode
typedef enum {
    UNFS_OPEN_RW        = 0x00,             ///< default is read/write
    UNFS_OPEN_CREATE    = 0x01,             ///< create if needed
    UNFS_OPEN_READONLY  = 0x02,             ///< open for read-only
    UNFS_OPEN_EXCLUSIVE = 0x40,             ///< open for exclusive access
} unfs_mode_t;

/// Data segment info
typedef struct {
    u64                 pageid;             ///< page address
    u64                 pagecount;          ///< page count
} unfs_ds_t;

/// File node in memory, where name will be allocated per string length,
/// directory node contains no segment, and file node will be 4k-page
typedef struct _unfs_node {
    // in-memory only fields
    char*               name;               ///< file name
    struct _unfs_node*  parent;             ///< parent node
    pthread_rwlock_t    lock;               ///< file lock
    u32                 open;               ///< open count
    u32                 memsize;            ///< node allocated size
    int                 updated;            ///< node persistent data updated
    // persistent fields
    u64                 pageid;             ///< page address
    u64                 parentid;           ///< parent page address
    u64                 size;               ///< file or directory size
    u32                 isdir;              ///< is a directory flag
    u32                 dscount;            ///< file data segment count
    unfs_ds_t           ds[0];              ///< file data segment array
} unfs_node_t;

/// File node as stored on device
typedef struct {
    union {
        unfs_node_t     node;               ///< file node structure
        unfs_page_t     page;               ///< file info page
    };
    unfs_page_t         name;               ///< file name page
} unfs_node_io_t;

/// Directory listing entry
typedef struct {
    char*               name;               ///< file/directory name
    u64                 size;               ///< file/directory size
    int                 isdir;              ///< directory flag
} unfs_dir_entry_t;

/// Directory listing content
typedef struct {
    char*               name;               ///< directory name
    u32                 size;               ///< list entry size
    unfs_dir_entry_t    list[0];            ///< list entries
} unfs_dir_list_t;

/// Client file/directory descriptor
typedef struct {
    int                 error;              ///< error number
    int                 mode;               ///< open mode
    void*               id;                 ///< id
} unfs_fd_t;

/// Device dependent IO context
typedef u32 unfs_ioc_t;

/// Client filesystem handle
typedef s64 unfs_fs_t;

/// Device I/O implementation structure
typedef struct {
    /// device name
    char*           name;
    /// close device
    void            (*close)();
    /// allocate IO context
    unfs_ioc_t      (*ioc_alloc)();
    /// free IO context
    void            (*ioc_free)(unfs_ioc_t ioc);
    /// allocate IO pages
    void*           (*page_alloc)(unfs_ioc_t ioc, u32* pc);
    /// free IO pages
    void            (*page_free)(unfs_ioc_t ioc, void* buf, u32 pc);
    /// read data from device into buffer
    void            (*read)(unfs_ioc_t ioc, void* buf, u64 pa, u32 pc);
    /// write data from buffer onto device
    void            (*write)(unfs_ioc_t ioc, const void* buf, u64 pa, u32 pc);
} unfs_device_io_t;

/// Filesystem header page layout (at lba 0)
typedef struct {
    union {
        unfs_page_t     info[UNFS_HEADPC];  ///< header info pages
        struct {
            char        label[64];          ///< disk label
            char        version[16];        ///< filesystem version name
            u64         blockcount;         ///< number of blocks
            u64         pagecount;          ///< total number of pages
            u64         pagefree;           ///< number of free pages
            u32         blocksize;          ///< block size
            u32         pagesize;           ///< page size
            u64         datapage;           ///< start data page address
            u64         fdnextpage;         ///< next file entry page address
            u64         fdcount;            ///< number of file entries
            u64         dircount;           ///< number of directories count
            u64         mapsize;            ///< map size in 64-bit word
            u32         delmax;             ///< deleted stack max size
            u32         delcount;           ///< deleted stack count
            u64         delstack[];         ///< stack of deleted file entries
        };
    };
    unfs_page_t         map[0];             ///< free bitmap page
} unfs_header_t;


// Export functions
int unfs_format(const char* device, const char* label, int print);
int unfs_check(const char* device);

unfs_fs_t unfs_open(const char* device);
int unfs_close(unfs_fs_t fs);

int unfs_create(unfs_fs_t fs, const char* name, int isdir, int pflag);
int unfs_remove(unfs_fs_t fs, const char* name, int isdir);
int unfs_rename(unfs_fs_t fs, const char* src, const char* dst, int override);
int unfs_exist(unfs_fs_t fs, const char* name, int* isdirp, u64* sizep);
int unfs_stat(unfs_fs_t fs, unfs_header_t* statp, int print);

unfs_dir_list_t* unfs_dir_list(unfs_fs_t fs, const char* name);
void unfs_dir_list_free(unfs_dir_list_t* listp);

unfs_fd_t unfs_file_open(unfs_fs_t fs, const char* name, unfs_mode_t mode);
int unfs_file_close(unfs_fd_t fd);
int unfs_file_sync(unfs_fd_t fd);
char* unfs_file_name(unfs_fd_t fd, char* name, int len);
int unfs_file_stat(unfs_fd_t fd, u64* sizep, u32* dscp, unfs_ds_t** dslp);
int unfs_file_resize(unfs_fd_t fd, u64 size, int* fill);
int unfs_file_read(unfs_fd_t fd, void *buf, u64 offset, u64 len);
int unfs_file_write(unfs_fd_t fd, const void *buf, u64 offset, u64 len);
u64 unfs_file_checksum(unfs_fd_t fd);

#endif	// _UNFS_H
