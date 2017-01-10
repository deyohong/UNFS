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
 * @brief Simple shell for invoking UNFS commands.
 */

#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <error.h>
#include <ctype.h>

#include "unfs.h"


/// Usage
static const char*  usage =
"\nUsage: %s [OPTION]... DEVICE_NAME\n\
          -n NSID       NVMe namespace id (default 1)\n\
          -h SIZE       command history size (default 100)\n\
          DEVICE_NAME   device name\n";

/// Help
static const char*  help =
"Available Commands:     (Ctrl-P=Previous  Ctrl-N=Next)\n\
---------------------------------------------------------------\n\
cd [DIRNAME]            touch FILENAME          cp FROM TO\n\
ls [DIRNAME]            rm FILENAME             mv FROM TO\n\
find [DIRNAME]          file FILENAME           cmp FILE1 FILE2\n\
mkdir DIRNAME           fs                      history\n\
rmdir DIRNAME           fsck                    q|quit|exit\n\
---------------------------------------------------------------\n";

static char*        device;                     ///< device name
static int          histsize = 100;             ///< history size
static int          histnext = 0;               ///< next history position
static unfs_page_t* history;                    ///< command history queue
static unfs_fs_t    fs;                         ///< filesystem handle
static char         cwd[UNFS_MAXPATH] = "/";    ///< current directory


/**
 * Invoke a command with 1 required argument.
 */
int run(int(*cmd)(const char*), const char* arg)
{
    char s[UNFS_MAXPATH];
    if (*arg != '/') {
        char* sep = cwd[1] ? "/" : "";
        snprintf(s, sizeof(s), "%s%s%s", cwd, sep, arg);
        arg = s;
    }
    return cmd(arg);
}

/**
 * Invoke a command with 1 required argument.
 */
int run2(int(*cmd)(const char*, const char*), const char* arg1, const char* arg2)
{
    char s1[UNFS_MAXPATH];
    char s2[UNFS_MAXPATH];
    char* sep = cwd[1] ? "/" : "";
    if (*arg1 != '/') {
        snprintf(s1, sizeof(s1), "%s%s%s", cwd, sep, arg1);
        arg1 = s1;
    }
    if (*arg2 != '/') {
        snprintf(s2, sizeof(s2), "%s%s%s", cwd, sep, arg2);
        arg2 = s2;
    }
    int isdir;
    if (unfs_exist(fs, arg2, &isdir, 0)) {
        if (isdir) {
            char* filename = strrchr(arg1, '/');
            if (arg2[1] == 0)
                arg2 = filename;
            else
                strcat((char*)arg2, filename);
        }
    }
    return cmd(arg1, arg2);
}

/**
 * cd - change current directory.
 */
static int cmd_cd(const char* arg)
{
    if (!unfs_exist(fs, arg, 0, 0)) {
        printf("No such directory %s\n", arg);
        return 1;
    }
    if (arg != cwd) strcpy(cwd, arg);
    return 0;
}

/**
 * Compare to sort directory list entries.
 */
static int ls_compare(const void* p1, const void* p2, void* arg)
{
    if (arg) {
        int* plen = arg;
        int n = strlen(((unfs_dir_entry_t*)p1)->name);
        if (n > *plen)
            *plen = n;
        n = strlen(((unfs_dir_entry_t*)p2)->name);
        if (n > *plen)
            *plen = n;
    }
    return strcmp(((unfs_dir_entry_t*)p1)->name, ((unfs_dir_entry_t*)p2)->name);
}

/**
 * ls - lists a directory content.
 */
static int cmd_ls(const char* arg)
{
    // get directory listing
    unfs_dir_list_t* dlp = unfs_dir_list(fs, arg);
    if (!dlp) {
        printf("No such directory %s\n", arg);
        return 1;
    }
    if (dlp->size == 0)
        return 0;

    unfs_dir_entry_t* dep = dlp->list;
    int llen = 0;
    qsort_r(dep, dlp->size, sizeof(unfs_dir_entry_t), ls_compare, &llen);
    size_t dlen = strlen(dlp->name);
    int i;
    for (i = 0; i < dlp->size; i++) {
        int n = strlen(dep->name);
        const char* name = dep->name + dlen;
        if (dlen > 1)
            name++;
        if (dep->isdir)
            printf("%s/", name);
        else
            printf("%s ", name);
        while (n++ < llen)
            printf(" ");
        printf("  (%lu)\n", dep->size);
        dep++;
    }
    unfs_dir_list_free(dlp);
    return 0;
}

/**
 * Recursive find directory listing.
 */
static int find_recursive(const char* dirname)
{
    unfs_dir_list_t* dlp = unfs_dir_list(fs, dirname);
    if (!dlp) {
        printf("No such directory %s\n", dirname);
        return 1;
    }

    if (dlp->size == 0)
        return 0;
    unfs_dir_entry_t* dep = dlp->list;
    qsort_r(dep, dlp->size, sizeof(unfs_dir_entry_t), ls_compare, 0);
    int i;
    for (i = 0; i < dlp->size; i++) {
        if (dep->isdir) {
            printf("%s/  (%lu)\n", dep->name, dep->size);
            if (find_recursive(dep->name))
                return 1;
        } else {
            printf("%s  (%lu)\n", dep->name, dep->size);
        }
        dep++;
    }
    unfs_dir_list_free(dlp);
    return 0;
}

/**
 * find - list all children of a directory (i.e. recursive ls);
 */
static int cmd_find(const char* arg)
{
    u64 size;
    int isdir = 0;
    if (!unfs_exist(fs, arg, &isdir, &size)) {
        printf("%s does not exist\n", arg);
        return 1;
    }
    if (!isdir) {
        printf("%s  (%lu)\n", arg, size);
        return 1;
    }

    return find_recursive(arg);
}

/**
 * mkdir - create a directory including the parent directories.
 */
static int cmd_mkdir(const char* arg)
{
    if (unfs_create(fs, arg, 1, 1)) {
        printf("Cannot create directory %s\n", arg);
        return 1;
    }
    return 0;
}

/**
 * rmdir - remove a directory.
 */
static int cmd_rmdir(const char* arg)
{
    int isdir;
    if (!unfs_exist(fs, arg, &isdir, 0)) {
        printf("No such directory %s\n", arg);
        return 1;
    }
    if (!isdir) {
        printf("%s is not a directory\n", arg);
        return 1;
    }
    if (unfs_remove(fs, arg, 1)) {
        printf("Cannot remove %s (directory may not be empty)\n", arg);
        return 1;
    }
    return 0;
}

/**
 * touch - create a file if it does not exist.
 */
static int cmd_touch(const char* arg)
{
    if (!unfs_exist(fs, arg, 0, 0)) {
        if (unfs_create(fs, arg, 0, 0)) {
            printf("Cannot create file %s\n", arg);
            return 1;
        }
    }
    return 0;
}

/**
 * rm - remove a file.
 */
static int cmd_rm(const char* arg)
{
    int isdir;
    if (!unfs_exist(fs, arg, &isdir, 0)) {
        printf("No such file %s\n", arg);
        return 1;
    }
    if (isdir) {
        printf("%s is not a file\n", arg);
        return 1;
    }
    if (unfs_remove(fs, arg, 0)) {
        printf("Cannot remove %s (file may be opened)\n", arg);
        return 1;
    }
    return 0;
}

/**
 * file - print a file status.
 */
static int cmd_file(const char* arg)
{
    int isdir;
    if (!unfs_exist(fs, arg, &isdir, 0)) {
        printf("No such file %s\n", arg);
        return 1;
    }
    if (isdir) {
        printf("%s is not a file\n", arg);
        return 1;
    }
    unfs_fd_t fd = unfs_file_open(fs, arg, 0);
    if (fd.error) {
        printf("Open %s (%s)\n", arg, strerror(fd.error));
        return 1;
    }
    u64 size;
    u32 dsc;
    unfs_ds_t* dslp;
    u64 sum = unfs_file_checksum(fd);
    unfs_file_stat(fd, &size, &dsc, &dslp);
    unfs_file_close(fd);
    printf("Size=%lu  Segment=%u  Checksum=%#lx\n", size, dsc, sum);
    if (dsc) {
        printf("\nSegment   \tPage     \tCount\n");
        printf("=======   \t====     \t=====\n");
        int i;
        for (i = 0; i < dsc; i++) {
            printf("DS[%d]:  \t%-#8lx \t%lu\n", i, dslp[i].pageid,
                                                   dslp[i].pagecount);
        }
    }
    free(dslp);
    return 0;
}

/**
 * mv - move a file or directory.
 */
static int cmd_mv(const char* arg1, const char* arg2)
{
    int isdir;
    if (!unfs_exist(fs, arg1, &isdir, 0)) {
        printf("No such file or directory %s\n", arg1);
        return 1;
    }
    if (unfs_exist(fs, arg2, &isdir, 0)) {
        if (isdir)
            strcat((char*)arg2, strrchr(arg1, '/'));
    }
    if (unfs_rename(fs, arg1, arg2, 1)) {
        printf("Cannot move %s to %s\n", arg1, arg2);
        return 1;
    }
    return 0;
}

/**
 * cmp - compare two files.
 */
static int cmd_cmp(const char* arg1, const char* arg2)
{
    unfs_fd_t fd1 = unfs_file_open(fs, arg1, 0);
    if (fd1.error) {
        printf("Open %s (%s)\n", arg1, strerror(fd1.error));
        return 1;
    }
    unfs_fd_t fd2 = unfs_file_open(fs, arg2, 0);
    if (fd2.error) {
        printf("Open %s (%s)\n", arg2, strerror(fd2.error));
        unfs_file_close(fd1);
        return 1;
    }

    u64 offset = 0;
    u64 size1, size2;
    if (unfs_file_stat(fd1, &size1, 0, 0))
        return 1;
    if (unfs_file_stat(fd2, &size2, 0, 0))
        return 1;
    if (size1 != size2) {
        printf("%s %lu and %s %lu differ\n", arg1, size1, arg2, size2);
        return 1;
    }

    u8 buf1[UNFS_PAGESIZE];
    u8 buf2[UNFS_PAGESIZE];
    while (size1) {
        u64 len = sizeof(buf1);
        if (len > size1)
            len = size1;
        unfs_file_read(fd1, buf1, offset, len);
        unfs_file_read(fd2, buf2, offset, len);

        u64 i;
        for (i = 0; i < len; i++) {
            if (buf1[i] != buf2[i]) {
                printf("%s %s differ at byte %lu\n", arg1, arg2, offset + i);
                return 1;
            }
        }

        offset += len;
        size1 -= len;
    }
    unfs_file_close(fd1);
    unfs_file_close(fd2);
    return 0;
}

/**
 * cp - copy a file.
 */
static int cmd_cp(const char* arg1, const char* arg2)
{
    if (unfs_exist(fs, arg2, 0, 0)) {
        printf("%s exists\n", arg2);
        return 1;
    }
    unfs_fd_t fd1 = unfs_file_open(fs, arg1, 0);
    if (fd1.error) {
        printf("Open %s (%s)\n", arg1, strerror(fd1.error));
        return 1;
    }
    unfs_fd_t fd2 = unfs_file_open(fs, arg2, UNFS_OPEN_CREATE);
    if (fd2.error) {
        printf("Create %s (%s)\n", arg2, strerror(fd2.error));
        unfs_file_close(fd1);
        return 1;
    }

    u64 offset = 0, size = 0;
    if (unfs_file_stat(fd1, &size, 0, 0))
        return 1;

    u8 buf[65536];
    while (size) {
        u64 len = sizeof(buf);
        if (len > size)
            len = size;
        unfs_file_read(fd1, buf, offset, len);
        unfs_file_write(fd2, buf, offset, len);
        offset += len;
        size -= len;
    }

    u32 dsc;
    unfs_file_stat(fd1, &size, &dsc, 0);
    u64 sum = unfs_file_checksum(fd1);
    printf("%s\n  Size=%lu  Segment=%u  Checksum=%#lx\n", arg1, size, dsc, sum);
    unfs_file_close(fd1);

    unfs_file_stat(fd2, &size, &dsc, 0);
    sum = unfs_file_checksum(fd2);
    printf("%s\n  Size=%lu  Segment=%u  Checksum=%#lx\n", arg2, size, dsc, sum);
    unfs_file_close(fd2);

    return 0;
}

/**
 * fs - print the filessytem info.
 */
static int cmd_fs(void)
{
    unfs_header_t hdr;
    if (unfs_stat(fs, &hdr, 1)) {
        printf("UNFS status error\n");
        return 1;
    }
    return 0;
}

/**
 * fsck - recheck the filesystem.
 */
static int cmd_fsck(void)
{
    unfs_close(fs);
    printf("Checking filesystem... ");
    fflush(stdout);
    if (unfs_check(device)) {
        printf("\nUNFS error");
        exit(1);
    }
    fs = unfs_open(device);
    if (!fs) {
        printf("\nUNFS open failed");
        exit(1);
    }
    printf("ok\n");
    return cmd_fs();
}

/**
 * Print command history.
 */
static int cmd_history(void)
{
    int pos = histnext;
    do {
        if (++pos >= histsize)
            pos = 0;
        if (*history[pos])
            printf("%s\n", history[pos]);
    } while (pos != histnext);
    return 0;
}

/**
 * Read a command line.
 */
static void prompt_cmd(char* command)
{
    int pos = histnext;
    char* s = command;
    *s = 0;

    printf("\nUNFS:%s> ", cwd);
    int len, c;
    while ((c = getchar()) != '\n') {
        if ((c == '\b' || c == 127) && s > command) {   // backspace
            printf("\b \b");
            s--;
        } else if (c == ('U' - '@')) {                  // Ctrl-U
            len = s - command;
            for (c = 0; c < len; c++)
                printf("\b \b");
            s = command;
        } else if (c == ('P' - '@')) {                  // Ctrl-P
            if (--pos < 0)
                pos = histsize - 1;
            if (*history[pos] == 0) {
                if (++pos >= histsize)
                    pos = 0;
            } else {
                len = s - command;
                for (c = 0; c < len; c++)
                    printf("\b \b");
                strcpy(command, history[pos]);
                printf(command);
                s = command + strlen(command);
            }
        } else if (c == ('N' - '@')) {                  // Ctrl-N
            if (++pos >= histsize)
                pos = 0;
            if (*history[pos] == 0) {
                if (--pos < 0) pos = histsize - 1;
            } else {
                len = s - command;
                for (c = 0; c < len; c++)
                    printf("\b \b");
                strcpy(command, history[pos]);
                printf(command);
                s = command + strlen(command);
            }
        } else if (isprint(c)) {
            putchar(c);
            *s++ = c;
        }
    }
    *s = 0;

    putchar('\n');
    if (*command) {
        pos = histnext;
        if (--pos < 0)
            pos = histsize - 1;
        if (strcmp(history[pos], command)) {
            strcpy(history[histnext], command);
            if (++histnext == histsize)
                histnext = 0;
        }
    }
}

/**
 * Main program.
 */
int main(int argc, char** argv)
{
    const char* prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];
    int opt;

    while ((opt = getopt(argc, argv, "n:h:")) != -1) {
        switch (opt) {
        case 'n':
            setenv("UNFS_NSID", optarg, 1);
            break;
        case 'h':
            histsize = atoi(optarg);
            if (histsize <= 0)
                error(1, 0, "history size must be > 0");
            break;
        default:
            error(1, 0, usage, prog);
        }
    }

    const char* name = getenv("UNFS_DEVICE");
    if (optind < argc)
        name = argv[optind];
    else if (!name)
        error(1, 0, usage, prog);
    device = strdup(name);

    history = (unfs_page_t*)calloc(histsize, sizeof(unfs_page_t));

    fs = unfs_open(device);
    if (!fs)
        error(1, 0, "UNFS open failed");

    printf("UNFS Shell (device %s)\n", device);

    int status = 0;
    struct termios oldterm, rawterm;
    tcgetattr(STDIN_FILENO, &oldterm);
    rawterm = oldterm;
    rawterm.c_lflag &= ~(ICANON|ECHO);   
    tcsetattr(STDIN_FILENO, TCSANOW, &rawterm);

    for (;;) {
        // prompt to read command and parse into arguments
        char command[UNFS_MAXPATH];
        prompt_cmd(command);
        char* cmdp = strtok(command, " \t\n");
        char* argp = strtok(0, " \t\n");
        char* argp2 = strtok(0, " \t\n");

        // if no command is given print help
        if (!cmdp) {
            printf(help);
            status = 0;
            continue;
        }

        // exit shell
        if (!strcmp(cmdp, "q") || !strcmp(cmdp, "quit") || !strcmp(cmdp, "exit"))
            break;

        // status command
        if (!strcmp(cmdp, "status")) {
            printf("%d\n", status);
            status = 0;

        // history command
        } else if (!strcmp(cmdp, "history")) {
            status = cmd_history();

        // cd command
        } else if (!strcmp(cmdp, "cd")) {
            if (!argp) argp = cwd;
            status = run(cmd_cd, argp);

        // ls command
        } else if (!strcmp(cmdp, "ls")) {
            if (!argp) argp = cwd;
            status = run(cmd_ls, argp);

        // find command
        } else if (!strcmp(cmdp, "find")) {
            if (!argp) argp = cwd;
            status = run(cmd_find, argp);

        // mkdir command
        } else if (!strcmp(cmdp, "mkdir")) {
            if (!argp) {
                printf("Syntax: mkdir DIRNAME\n");
                status = 1;
                continue;
            }
            status = run(cmd_mkdir, argp);

        // rmdir command
        } else if (!strcmp(cmdp, "rmdir")) {
            if (!argp) {
                printf("Syntax: rmdir DIRNAME\n");
                status = 1;
                continue;
            }
            status = run(cmd_rmdir, argp);

        // touch command
        } else if (!strcmp(cmdp, "touch")) {
            if (!argp) {
                printf("Syntax: touch FILENAME\n");
                status = 1;
                continue;
            }
            status = run(cmd_touch, argp);

        // rm command
        } else if (!strcmp(cmdp, "rm")) {
            if (!argp) {
                printf("Syntax: rm FILENAME\n");
                status = 1;
                continue;
            }
            status = run(cmd_rm, argp);

        // file command
        } else if (!strcmp(cmdp, "file")) {
            if (!argp) {
                printf("Syntax: file FILENAME\n");
                status = 1;
                continue;
            }
            status = run(cmd_file, argp);

        // mv command
        } else if (!strcmp(cmdp, "mv")) {
            if (!argp || !argp2) {
                printf("Syntax: mv FROM TO\n");
                status = 1;
                continue;
            }
            status = run2(cmd_mv, argp, argp2);

        // cp command
        } else if (!strcmp(cmdp, "cp")) {
            if (!argp || !argp2) {
                printf("Syntax: cp FROM TO\n");
                status = 1;
                continue;
            }
            status = run2(cmd_cp, argp, argp2);

        // cmp command
        } else if (!strcmp(cmdp, "cmp")) {
            if (!argp || !argp2) {
                printf("Syntax: cmp FILE1 FILE2\n");
                status = 1;
                continue;
            }
            status = run2(cmd_cmp, argp, argp2);

        // fs command
        } else if (!strcmp(cmdp, "fs")) {
            status = cmd_fs();

        // fsck command
        } else if (!strcmp(cmdp, "fsck")) {
            status = cmd_fsck();

        // unknown command
        } else {
            printf(help);
            status = 0;
        }
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &oldterm);

    unfs_close(fs);
    free(device);
    return 0;
}
