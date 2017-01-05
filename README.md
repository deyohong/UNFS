UNFS - User Space Nameless Filesystem
=====================================

UNFS is a user space filesystem developed at Miron Technology.
It is designed as a proof of concept to enable applications that
have dependency on filesystem to work with UNVMe driver.

UNFS is implemented as a library with APIs and a set of commands to format,
check, and examine the filesystem.

UNFS can be run with the user-space UNVMe driver (i.e. for NVMe based
devices) or against any raw block device supported by a kernel space driver.

This project also contains a plugin implementation for the WiredTiger
custom filesystem which enabled MongoDB to work on UNFS.  Note that
WiredTiger is the default data engine for MongoDB.



System Requirements
===================

UNFS is implemented on Linux CentOS 6 and 7 running on x86_64 based systems.
Note that root privilege is required to access raw device.

Although UNFS can be configured to be standalone, for completeness,
it requires additional source packages to be downloaded including UNVMe
driver, WiredTiger with custom filesystem feature support, and MongoDB.
Each of these packages may have its own requirements.  For example,
UNVMe requires Linux with a kernel version that supports VFIO, and
MongoDB requires at least GCC version 5.3 (which does not come with
CentOS 6 or 7), etc.



Build Packages
==============

To run MongoDB on UNFS, use the following procedure (tested on CentOS):

    1) Download UNFS code (assume the base working directory is /WORK):

        $ cd /WORK
        $ git clone https://github.com/MicronSSD/unfs.git


       To run UNFS wthout UNVMe using only raw block device, comment
       out "CPPFLAGS += -DUNFS_UNVME" in Makefile.def, in which case,
       skip Step 2 below.

       To run UNFS without WiredTiger and MongoDB, comment out "EXTDIRS"
       in Makefile.def, in which case, skip Step 3 and 5 below.


    2) Download and install UNVMe driver:

        $ cd /WORK
        $ git clone https://github.com/MicronSSD/unvme.git
        $ cd unvme
        $ make install


    3) Download, patch and build WiredTiger 'develop' branch code:

        $ cd /WORK
        $ git clone https://github.com/wiredtiger/wiredtiger.git -b develop
        $ cd wiredtiger

       Patch the WiredTiger code for UNFS usage:

        $ patch -p1 < /WORK/unfs/wiredtiger/unfs_wiredtiger.patch

       And then build the WiredTiger code with snappy:

        $ ./autogen.sh
        $ ./configure --with-builtins=snappy
        $ make -j16
        $ make install


    4) Build and install UNFS:

        $ cd /WORK/unfs
        $ make install


    5) Download and build MongoDB to use the custom WiredTiger engine
       with UNFS (bypassing the WiredTiger version that comes with MongoDB):

        $ cd /WORK
        $ git clone https://github.com/mongodb/mongo.git -b v3.4
        $ cd mongo
        $ scons LIBS="libunfswt libwiredtiger" --use-system-wiredtiger -j16



Run UNFS Tests
==============

When running UNFS applications, the target device name may be specified
by setting the environment variable UNFS_DEVICE (or as argument to a
given supported command).  UNFS will automatically bind to UNVMe driver
for device name with PCI format (e.g. 07:00.0) and to raw block device
for name starting with /dev/.


To use UNVMe driver for NVMe devices, run the setup script once:

    $ unvme_setup


The following UNFS unit tests may be run from source directory:

    $ test/unfs_rmw_test
    $ test/unfs_tree_test
    $ wiredtiger/unfs_wt_test


The following UNFS commands are available (in /usr/local/bin):

    unfs_format     - Format UNFS filesystem
    unfs_check      - Scan and validate UNFS filesystem nodes
    unfs_shell      - Program supporting some basic filesystem commands


Note that the libraries are installed in /usr/local/lib which may not be
in the default dynamic load paths.  If executables fail to run with
cannot open shared object file, try to add it to /etc/ld.so.conf, e.g.:

    $ echo /usr/local/lib >> /etc/ld.so.conf
    $ cat /etc/ld.so.conf
    $ ldconfig



Run MongoDB Tests
=================

The WiredTiger custom filesystem feature is still under development and
not yet released with MongoDB, so some MongoDB functionalities may not work,
especially where POSIX APIs are used to access native files and directories.
MongoDB 3.4 core features, however, have been tested with UNFS.

Steps to setup UNFS (with UNVMe) and run mongod:

    $ unvme-setup
    # Setup all NVMe devices for UNVMe driver
    07:00.0 Intel Corporation DC P3700 SSD [2.5" SFF] - (enabled for UNVMe)

    $ export UNFS_DEVICE=07:00.0

    $ unfs_format
    UNFS format device 07:00.0 label "User Space Nameless Filesystem"

    $ /WORK/mongod
    ...

Note that running unfs_format is equivalent to invoking mkfs on a new device.
For using raw device, skip unvme-setup and specify the device name instead,
e.g., export UNFS_DEVICE=/dev/nvme0n1 (or whatever block device name).


Steps to run MongoDB smoke tests:

    $ cd /WORK/mongo

    $ buildscripts/resmoke.py --storageEngine=wiredTiger --suites=core

It should be noted that the UNVMe driver only suports one process accessing
a given NVMe device, so the smoke test will fail at jsHeapLimit.js since the
test will launch a second mongod process and fail to open the NVMe device
that is already owned by another process.  However, all the core tests
should pass running against a raw device (using the kernel space driver).

