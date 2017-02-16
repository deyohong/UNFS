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
 * @brief WiredTiger custom filesystem plugin implementation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <search.h>
#include <string.h>

#include "unfs.h"
#include "unfs_log.h"
#include "unfs_wt.h"


/// WT custom file handle wrapper
typedef struct {
    WT_FILE_HANDLE          wtfh;           ///< WT file handle object
    unfs_fd_t               unfd;           ///< UNFS file descriptor reference
    int                     isdir;          ///< directory flag
    pthread_spinlock_t      lock;           ///< file lock
} unfs_wt_file_handle_t;

/// WT custom file system wrapper
typedef struct {
    WT_FILE_SYSTEM          wtfs;           ///< WT filesystem object
    unfs_fs_t               unfs;           ///< UNFS filesystem reference
    char*                   homedir;        ///< home directory absolute path
    const char*             home;           ///< home relative path name
} unfs_wt_file_system_t;

/// File type name (only for debugging purpose)
#ifdef UNFS_DEBUG
static const char* wt_file_types[] = { "CHECKPOINT", "DATA", "DIRECTORY",
                                       "LOG", "REGULAR" };
#endif


/**
 * Convert a file name to its full canonical path name.
 * @param   result      canonical path name result
 * @param   n           max result string length
 * @param   homedir     home directory absolute path
 * @param   prefix      WiredTiger home prefix path to be removed
 * @param   name        relative path name
 * @return  the result pointer.
 */
static char* unfs_wt_path(char* result, size_t n, const char* homedir,
                          const char* prefix, const char* name)
{
    *result = 0;
    if (name[0] == '/') {
        strncpy(result, name, n);
    } else {
        // fix WiredTiger prefix semantic
        int plen = strlen(prefix);
        if (plen && strncmp(name, prefix, plen) == 0 && name[plen] == '/')
            name += plen + 1;
        if (snprintf(result, n, "%s/%s", homedir, name) >= n) {
            ERROR("name %s is too long", name);
            abort();
        }
    }

    char* s = result;
    while ((s = strchr(s, '/')) != 0) {
        if (s[1] == 0) {
            *s = 0;
        } else if (s[1] == '/') {
            memmove(s, s+1, strlen(s+1) + 1);
        } else if (s[1] == '.') {
            if ((s[2] == '/') || (s[2] == 0)) {
                memmove(s, s+2, strlen(s+2) + 1);
            } else if ((s[2] == '.') && ((s[3] == '/') || (s[3] == 0))) {
                char* bs = s - 1;
                while (*bs != '/') bs--;
                memmove(bs, s+3, strlen(s+3) + 1);
                s = bs;
            }
        } else {
            s++;
        }
    }
    if (*result != '/') {
        result[0] = '/';
        result[1] = 0;
    }

    return result;
}

/**
 * Lock/Unlock a file.
 * @param   fh          WT file handle
 * @param   ses         WT session
 * @param   lock        to lock or unlock
 * @return  0 if ok else error code.
 */
static int unfs_wt_file_lock(WT_FILE_HANDLE *fh, WT_SESSION *ses, bool lock)
{
    DEBUG_FN("%s %d", fh->name, lock);
    unfs_wt_file_handle_t* uwfh = (unfs_wt_file_handle_t*)fh;
    if (lock) {
        pthread_spin_lock(&uwfh->lock);
    } else {
        pthread_spin_unlock(&uwfh->lock);
    }
    return 0;
}

/**
 * Get the file current size.
 * @param   fh          WT file handle
 * @param   ses         WT session
 * @param   sizep       returned size
 * @return  0 if ok else error code.
 */
static int unfs_wt_file_size(WT_FILE_HANDLE *fh, WT_SESSION *ses, wt_off_t *sizep)
{
    unfs_wt_file_handle_t* uwfh = (unfs_wt_file_handle_t*)fh;
    u64 size = 0;
    int err = unfs_file_stat(uwfh->unfd, &size, 0, 0);
    *sizep = size;
    DEBUG_FN("%s %#lx", fh->name, *sizep);
    return err;
}

/**
 * Truncate file to specified size.
 * @param   fh          WT file handle
 * @param   ses         WT session
 * @param   len         new size
 * @return  0 if ok else error code.
 */
static int unfs_wt_file_truncate(WT_FILE_HANDLE *fh, WT_SESSION *ses, wt_off_t len)
{
    DEBUG_FN("%s %#lx", fh->name, len);
    unfs_wt_file_handle_t* uwfh = (unfs_wt_file_handle_t*)fh;
    return unfs_file_resize(uwfh->unfd, len, 0);
}

/**
 * Read data from a file into a user buffer.
 * @param   fh          WT file handle
 * @param   ses         WT session
 * @param   offset      file offset
 * @param   len         number of bytes to read
 * @param   buf         the data buffer
 * @return  0 if ok else error code.
 */
static int unfs_wt_file_read(WT_FILE_HANDLE *fh, WT_SESSION *ses,
                             wt_off_t offset, size_t len, void *buf)
{
    DEBUG_FN("%s %#lx %#lx", fh->name, offset, len);
    unfs_wt_file_handle_t* uwfh = (unfs_wt_file_handle_t*)fh;
    return unfs_file_read(uwfh->unfd, buf, offset, len);
}

/**
 * Write data into a file.
 * @param   fh          WT file handle
 * @param   ses         WT session
 * @param   offset      file offset
 * @param   len         number of bytes to write
 * @param   buf         the data buffer
 * @return  0 if ok else error code.
 */
static int unfs_wt_file_write(WT_FILE_HANDLE *fh, WT_SESSION *ses,
                              wt_off_t offset, size_t len, const void *buf)
{
    DEBUG_FN("%s %#lx %#lx", fh->name, offset, len);
    unfs_wt_file_handle_t* uwfh = (unfs_wt_file_handle_t*)fh;
    return unfs_file_write(uwfh->unfd, buf, offset, len);
}

/**
 * Sync file.
 * @param   fh          WT file handle
 * @param   ses         WT session
 * @return  0 if ok else error code.
 */
static int unfs_wt_file_sync(WT_FILE_HANDLE *fh, WT_SESSION *ses)
{
    if (!fh->file_system) return 0;
    DEBUG_FN("%s", fh->name);
    unfs_wt_file_handle_t* uwfh = (unfs_wt_file_handle_t*)fh;
    return unfs_file_sync(uwfh->unfd);
}

/**
 * Close a file.
 * @param   fh          WT file handle
 * @param   ses         WT session
 * @return  0 if ok else error code.
 */
static int unfs_wt_file_close(WT_FILE_HANDLE *fh, WT_SESSION *ses)
{
    DEBUG_FN("%s", fh->name);
    unfs_wt_file_handle_t* uwfh = (unfs_wt_file_handle_t*)fh;
    int err = 0;
    if (!uwfh->isdir) {
        if (!(err = unfs_file_close(uwfh->unfd))) {
            (void) pthread_spin_destroy(&uwfh->lock);
            free(uwfh->wtfh.name);
            free(uwfh);
        }
    }
    return err;
}

/**
 * Open a file.
 * @param   fs          WT filesystem
 * @param   ses         WT session
 * @param   name        file name
 * @param   type        file type
 * @param   flags       flags
 * @param   fh          returned file handle (out)
 * @return  0 if ok else error code.
 */
static int unfs_wt_fs_open(WT_FILE_SYSTEM* fs, WT_SESSION *ses,
                           const char *name, WT_FS_OPEN_FILE_TYPE type,
                           uint32_t flags, WT_FILE_HANDLE** fh)
{
    static unfs_wt_file_handle_t unfs_wt_dir = {
                                    .isdir = 1,
                                    .wtfh.fh_lock = unfs_wt_file_lock,
                                    .wtfh.fh_read = unfs_wt_file_read,
                                    .wtfh.fh_write = unfs_wt_file_write,
                                    .wtfh.fh_size = unfs_wt_file_size,
                                    .wtfh.fh_sync = unfs_wt_file_sync,
                                    .wtfh.close = unfs_wt_file_close };

    DEBUG_FN("%s type=%s(%d) flags=%#x", name, wt_file_types[type], type, flags);
    *fh = 0;
    unfs_wt_file_system_t* unfs = (unfs_wt_file_system_t*)fs;
    char path[UNFS_MAXPATH];
    unfs_wt_path(path, sizeof(path)-1, unfs->homedir, unfs->home, name);

    if (type == WT_FS_OPEN_FILE_TYPE_DIRECTORY) {
        if (flags & WT_FS_OPEN_CREATE) {
            if (unfs_create(unfs->unfs, path, 1, 1)) {
                ERROR("Cannot create directory %s", path);
                return EINVAL;
            }
        }
        *fh = (WT_FILE_HANDLE*)&unfs_wt_dir;
        return 0;
    }

    unfs_mode_t mode = 0;
    if (flags & WT_FS_OPEN_CREATE)
        mode |= UNFS_OPEN_CREATE;
    if (flags & WT_FS_OPEN_EXCLUSIVE)
        mode |= UNFS_OPEN_EXCLUSIVE;

    unfs_fd_t fd = unfs_file_open(unfs->unfs, path, mode);
    if (fd.error) {
        ERROR("%s (%s)", path, strerror(fd.error));
        return fd.error;
    }

    unfs_wt_file_handle_t* uwfh = calloc(1, sizeof(unfs_wt_file_handle_t));
    (void) pthread_spin_init(&uwfh->lock, PTHREAD_PROCESS_SHARED);
    uwfh->wtfh.file_system = fs;
    uwfh->wtfh.name = strdup(name);
    uwfh->wtfh.fh_lock = unfs_wt_file_lock;
    uwfh->wtfh.fh_read = unfs_wt_file_read;
    uwfh->wtfh.fh_write = unfs_wt_file_write;
    uwfh->wtfh.fh_size = unfs_wt_file_size;
    uwfh->wtfh.fh_sync = unfs_wt_file_sync;
    uwfh->wtfh.fh_truncate = unfs_wt_file_truncate;
    uwfh->wtfh.close = unfs_wt_file_close;
    /*
    uwfh->wtfh.fh_advise = 0;
    uwfh->wtfh.fh_allocate = 0;
    uwfh->wtfh.fh_allocate_nolock = 0;
    uwfh->wtfh.fh_map = 0;
    uwfh->wtfh.fh_map_discard = 0;
    uwfh->wtfh.fh_map_preload = 0;
    uwfh->wtfh.fh_unmap = 0;
    uwfh->wtfh.fh_sync_nowait = 0;
     */
    uwfh->unfd = fd;
    *fh = (WT_FILE_HANDLE*)uwfh;
    return 0;
}

/**
 * Check if a filename exists.
 * @param   fs          WT filesystem
 * @param   ses         WT session
 * @param   name        name
 * @param   existp      returned exist flag (out)
 * @return  0 if ok else error code.
 */
static int unfs_wt_fs_exist(WT_FILE_SYSTEM* fs, WT_SESSION *ses,
                            const char *name, bool* existp)
{
    *existp = 0;
    unfs_wt_file_system_t* unfs = (unfs_wt_file_system_t*)fs;
    char path[UNFS_MAXPATH];
    unfs_wt_path(path, sizeof(path), unfs->homedir, unfs->home, name);
    *existp = unfs_exist(unfs->unfs, path, 0, 0);
    DEBUG_FN("%s %d", name, *existp);
    return 0;
}

/**
 * Remove a file from the filesystem.
 * @param   fs          WT filesystem
 * @param   ses         WT session
 * @param   name        file name
 * @param   flags       0 or WT_FS_DURABLE
 * @return  0 if ok else error code.
 */
static int unfs_wt_fs_remove(WT_FILE_SYSTEM* fs, WT_SESSION *ses,
                             const char *name, uint32_t flags)
{
    DEBUG_FN("%s durable=%d", name, flags);
    unfs_wt_file_system_t* unfs = (unfs_wt_file_system_t*)fs;
    char path[UNFS_MAXPATH];
    unfs_wt_path(path, sizeof(path), unfs->homedir, unfs->home, name);
    return unfs_remove(unfs->unfs, path, 0);
}

/**
 * Rename a file in the filesystem.
 * @param   fs          WT filesystem
 * @param   ses         WT session
 * @param   from        from file name
 * @param   to          to file name
 * @param   flags       0 or WT_FS_DURABLE
 * @return  0 if ok else error code.
 */
static int unfs_wt_fs_rename(WT_FILE_SYSTEM* fs, WT_SESSION *ses,
                             const char *from, const char *to, uint32_t flags)
{
    DEBUG_FN("%s %s durable=%d", from, to, flags);
    unfs_wt_file_system_t* unfs = (unfs_wt_file_system_t*)fs;
    char fromname[UNFS_MAXPATH];
    unfs_wt_path(fromname, sizeof(fromname), unfs->homedir, unfs->home, from);
    char toname[UNFS_MAXPATH];
    unfs_wt_path(toname, sizeof(toname), unfs->homedir, unfs->home, to);
    return unfs_rename(unfs->unfs, fromname, toname, 1);
}

/**
 * Get the current size of the named file.
 * @param   fs          WT filesystem
 * @param   ses         WT session
 * @param   name        file name
 * @param   sizep       returned size (out)
 * @return  0 if ok else error code.
 */
static int unfs_wt_fs_size(WT_FILE_SYSTEM* fs, WT_SESSION *ses, const char *name,
                           wt_off_t *sizep)
{
    unfs_wt_file_system_t* unfs = (unfs_wt_file_system_t*)fs;
    char path[UNFS_MAXPATH];
    unfs_wt_path(path, sizeof(path), unfs->homedir, unfs->home, name);
    int isdir;
    u64 size;
    if (unfs_exist(unfs->unfs, path, &isdir, &size)) {
        *sizep = size;
        DEBUG_FN("%s%s %#lx", name, isdir ? "/" : "", *sizep);
        return 0;
    }
    ERROR("%s not found", name);
    return ENOENT;
}

/**
 * Get a directory listing.  The directory array and filenames will be
 * allocated here and need to be freed by the caller.
 * @param   fs          WT filesystem
 * @param   ses         WT session
 * @param   dirname     directory name
 * @param   prefix      prefix to match filename
 * @param   dirlistp    returned an allocated directory name list (out)
 * @param   countp      returned number of names (out)
 * @return  0 if ok else error code.
 */
static int unfs_wt_fs_directory_list(WT_FILE_SYSTEM* fs, WT_SESSION* ses,
                                     const char* dirname, const char* prefix,
                                     char*** dirlistp, uint32_t* countp)
{
    DEBUG_FN("%s %s", dirname, prefix);
    *countp = 0;
    *dirlistp = 0;
    unfs_wt_file_system_t* unfs = (unfs_wt_file_system_t*)fs;
    char path[UNFS_MAXPATH];
    unfs_wt_path(path, sizeof(path)-1, unfs->homedir, unfs->home, dirname);

    unfs_dir_list_t* dlp = unfs_dir_list(unfs->unfs, path);
    if (!dlp) {
        ERROR("No such directory %s", path);
        return ENOENT;
    }
    char** namelist = calloc(dlp->size + 1, sizeof(char*));
    size_t dirlen = strlen(dlp->name);
    size_t prefixlen = prefix ? strlen(prefix) : 0;
    int i, count = 0;
    for (i = 0; i < dlp->size; i++) {
        char* name = dlp->list[i].name + dirlen;
        if (dirlen > 1)
            name++;
        if (!prefixlen || !strncmp(name, prefix, prefixlen)) {
            namelist[count++] = name;
            DEBUG_FN("%s %s (%d)", dirname, name, count);
        }
    }
    *countp = count;
    if (count) {
        namelist[count] = (char*)dlp;   // save dlp to free it later
        *dirlistp = namelist;
    } else {
        free(namelist);
        unfs_dir_list_free(dlp);
        *dirlistp = NULL;
    }
    return 0;
}

/**
 * Free memory allocated by directory unfs_wt_fs_directory_list.
 * @param   fs          WT filesystem
 * @param   ses         WT session
 * @param   namelist    directory name list to free
 * @param   count       directory list entry count
 * @return  0 if ok else error code.
 */
static int unfs_wt_fs_directory_list_free(WT_FILE_SYSTEM* fs, WT_SESSION *ses,
                                          char **namelist, uint32_t count)
{
    if (namelist) {
        unfs_dir_list_t* dlp = (unfs_dir_list_t*)namelist[count];
        DEBUG_FN("%d (%s %d)", count, dlp->name, dlp->size);
        free(namelist);
        unfs_dir_list_free(dlp);
    }
    return 0;
}

/**
 * Terminate a filesystem and cleanup.
 * @param   fs          WT filesystem
 * @param   ses         WT session
 * @return  0 if ok else error code.
 */
static int unfs_wt_fs_terminate(WT_FILE_SYSTEM* fs, WT_SESSION* ses)
{
    unfs_wt_file_system_t* unfs = (unfs_wt_file_system_t*)fs;
    DEBUG_FN();
    unfs_close(unfs->unfs);
    free(unfs->homedir);
    free(unfs);
    return 0;
}

/**
 * Initialize WiredTiger custom UNFS filesystem.
 * @param   conn        connection
 * @param   config      config argument
 * @return  0 if ok else error code.
 */
int unfs_wt_init(WT_CONNECTION *conn, WT_CONFIG_ARG *config)
{
    // parse config arguments
    int err;
    WT_CONFIG_PARSER *parser;
    WT_CONFIG_ITEM key, val;
    WT_EXTENSION_API* wtext = conn->get_extension_api(conn);

    if ((err = wtext->config_parser_open_arg(wtext, NULL, config, &parser))) {
        ERROR("config_parser_open_arg: %s", wiredtiger_strerror(err));
        return err;
    }

    while ((err = parser->next(parser, &key, &val)) == 0) {
        if (strncmp("device", key.str, key.len) == 0) {
            setenv("UNFS_DEVICE", val.str, 1);
        } else if (strncmp("nsid", key.str, key.len) == 0) {
            setenv("UNFS_NSID", val.str, 1);
        } else if (strncmp("qcount", key.str, key.len) == 0) {
            setenv("UNFS_QCOUNT", val.str, 1);
        } else if (strncmp("qdepth", key.str, key.len) == 0) {
            setenv("UNFS_QDEPTH", val.str, 1);
        } else {
            ERROR("unknown config: %s", key.str);
            return EINVAL;
        }
    }

    if (err != WT_NOTFOUND) {
        ERROR("parser.next: %s", wiredtiger_strerror(err));
        return err;
    }
    if ((err = parser->close(parser))) {
        ERROR("parser.close: %s", wiredtiger_strerror(err));
        return err;
    }

    char* device = getenv("UNFS_DEVICE");
    if (!device) {
        ERROR("missing device name");
        return EINVAL;
    }

    // canonicalize the home directory path
    const char* home = conn->get_home(conn);
    if (!home) {
        ERROR("missing home directory");
        return EINVAL;
    }
    char homedir[UNFS_MAXPATH] = "/";
    unfs_wt_path(homedir, sizeof(homedir), "/", "", home);

    // open the UNFS filesystem
    unfs_fs_t fs = unfs_open(device);
    if (!fs) {
        ERROR("unfs_open %s failed", device);
        return ENODEV;
    }

    // create and register WT custom filesystem
    unfs_wt_file_system_t* unfs = calloc(1, sizeof(unfs_wt_file_system_t));
    unfs->unfs = fs;
    unfs->home = home;
    unfs->homedir = strdup(homedir);
    unfs->wtfs.fs_directory_list = unfs_wt_fs_directory_list;
    unfs->wtfs.fs_directory_list_free = unfs_wt_fs_directory_list_free;
    unfs->wtfs.fs_exist = unfs_wt_fs_exist;
    unfs->wtfs.fs_open_file = unfs_wt_fs_open;
    unfs->wtfs.fs_remove = unfs_wt_fs_remove;
    unfs->wtfs.fs_rename = unfs_wt_fs_rename;
    unfs->wtfs.fs_size = unfs_wt_fs_size;
    unfs->wtfs.terminate = unfs_wt_fs_terminate;

    if ((err = conn->set_file_system(conn, &unfs->wtfs, 0)) != 0) {
        ERROR("set_file_system: %s", wiredtiger_strerror(err));
        return err;
    }

    // workaround MongoDB bug SERVER-27571
    strcat(homedir, "/journal");
    if (unfs_create(fs, homedir, 1, 1)) {
        ERROR("Cannot create directory %s", homedir);
        return EINVAL;
    }

    return 0;
}

/**
 * Allocate and convert a wiredtiger config string that points to UNFS.
 * The returned config string must be freed by caller.
 * @param   home            home directory
 * @param   config          original config string
 * @return  newly config string for UNFS or NULL if error.
 */
char* unfs_wt_config(const char* home, const char* config)
{
    if (!config)
        config = "";
    size_t len = strlen(config);
    char* newconfig = malloc(len + 64);
    strcpy(newconfig, config);

    // check if string has already been converted
    if (!strstr(newconfig, "unfs_wt_init")) {
        // lazy check for existing extensions to append or create string
        const char* ext = strstr(config, "extensions=[");
        if (ext) {
            sprintf(newconfig + (ext - config + 12), "libunfswt.so="
                    "{entry=unfs_wt_init,early_load=true},%s]", ext + 12);
        } else {
            sprintf(newconfig + len, ",extensions=[libunfswt.so="
                    "{entry=unfs_wt_init,early_load=true}]");
        }
        DEBUG_FN("OLD: %s", config);
        DEBUG_FN("NEW: %s", newconfig);
    }

    return newconfig;
}

/**
 * Wrapper function for wiredTiger_open().  There are 3 ways to change
 * WiredTiger to work with UNFS instead of local filesystem:
 * 
 * 1) Invoke wiredtiger_open() with a config string that points to
 *    unfs_wiredtiger_init() and UNFS device.
 * 
 * 2) Change wiredtiger_open() to invoke unfs_wiredtiger_config() to convert
 *    the config string at the beginning of the wiredtiger_open() function.
 * 
 * 3) Replace all wiredtiger_open() calls in applications to use this wrapper
 *    function unfs_wiredtiger_open() in which it will convert the config
 *    string to point to unfs_wiredtiger_init().
 * 
 * @param   home        home directory
 * @param   errhandler  error handler
 * @param   config      config string
 * @param   conn        connection pointer
 * @return  0 if ok else error code.
 */
int unfs_wt_open(const char* home, WT_EVENT_HANDLER* errhandler,
                         const char* config, WT_CONNECTION** conn)
{
    char* newconfig = unfs_wt_config(home, config);
    int err = wiredtiger_open(home, errhandler, newconfig, conn);
    free(newconfig);
    return err;
}

