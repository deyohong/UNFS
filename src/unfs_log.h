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
 * @brief UNFS logging header file.
 */

#ifndef _UNFS_LOG_H
#define _UNFS_LOG_H

/// @cond

#ifdef  UNFS_UNVME
    #ifdef UNFS_DEBUG
        #define UNVME_DEBUG
    #endif
    #include <unvme_log.h>

    #define LOG_OPEN()              log_open("/dev/shm/unfs.log", "w");
    #define LOG_CLOSE()             log_close();
#else
    #include <stdio.h>

    #define LOG_OPEN()
    #define LOG_CLOSE()

    #define INFO(fmt, arg...)       printf(fmt "\n", ##arg)
    #define INFO_FN(fmt, arg...)    printf("%s " fmt "\n", __func__, ##arg)
    #define ERROR(fmt, arg...)      fprintf(stderr, "ERROR: %s " fmt "\n", __func__, ##arg)

    #ifdef UNFS_DEBUG
        #define DEBUG               INFO
        #define DEBUG_FN            INFO_FN
    #else
        #define DEBUG(arg...)
        #define DEBUG_FN(arg...)
    #endif
#endif // _UNVME_LOG_H

/// @endcond

#endif // _UNFS_LOG_H

