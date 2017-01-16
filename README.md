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

To run MongoDB on UNFS, use the following procedure (as tested on CentOS):

    1) Download UNFS code (assume the base working directory is /WORK):

        $ cd /WORK
        $ git clone https://github.com/MicronSSD/unfs.git


       To build UNFS wthout UNVMe, i.e. using only raw block device,
       comment out "CPPFLAGS += -DUNFS_UNVME" in Makefile.def.

       To build UNFS without WiredTiger, comment out the "EXTDIRS"
       list in Makefile.def, in which case, skip Step 3 and 5 below.


    2) Download and install UNVMe driver:

        $ cd /WORK
        $ git clone https://github.com/MicronSSD/unvme.git
        $ cd unvme
        $ make install


    3) Download, patch and build WiredTiger 'develop' branch code:

        $ cd /WORK
        $ git clone https://github.com/wiredtiger/wiredtiger.git -b develop
        $ cd wiredtiger

       Patch the WiredTiger code for UNFS support:

        $ patch -p1 -i /WORK/unfs/wiredtiger/unfs_wiredtiger.patch

       And then build the WiredTiger code with snappy and zlib enabled:

        $ ./autogen.sh
        $ ./configure --with-builtins=snappy,zlib
        $ make -j16
        $ make install


    4) Now build UNFS:

        $ cd /WORK/unfs
        $ make install


    5) Download and build MongoDB to use the custom WiredTiger engine
       with UNFS (overriding the WiredTiger code that comes with MongoDB):

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
    07:00.0 Intel Corporation DC P3700 SSD [2.5" SFF] - (enabled for UNVMe)

    $ export UNFS_DEVICE=07:00.0

    $ unfs_format
    UNFS format device 07:00.0 label "User Space Nameless Filesystem"

    $ /WORK/mongod
    ...

Note that running unfs_format is equivalent to invoking mkfs on a new device.

To run UNFS with raw device, invoke command 'unvme-setup reset' to restore
the binding of NVMe device to the kernel space driver, and then export the
the actual device name (e.g. /dev/nvme0n1) instead, e.g.,
export UNFS_DEVICE=/dev/nvme0n1.


Steps to run MongoDB smoke tests:

    $ cd /WORK/mongo
    $ buildscripts/resmoke.py --storageEngine=wiredTiger --suites=core
    $ buildscripts/resmoke.py --storageEngine=wiredTiger --suites=dbtest
    $ buildscripts/resmoke.py --storageEngine=wiredTiger --suites=unittests


It should be noted that the UNVMe driver only suports one process accessing
a given NVMe device, so the smoke test will fail at jsHeapLimit.js (core suite)
since the test will launch a second mongod process and thus fail to open the
NVMe device that is already owned by another process.  To run the core suite
successfully using UNVMe device, remove jsHeapLimit.js from the jstests/core
directory.  The jsHeapLimit.js problem does not apply to UNFS raw device.

It should also be noted that running these small tests using UNVMe driver 
will take many times longer.  This is because each individual test application
will have to first load the user space driver and initialize the device
every time before the actual test is run.



Run YCSB Benchmarks
===================

Assume YCSB 0.12.0 is already installed on the system (in /opt/ycsb),
a shell script unfs-mongo-ycsb is provided for running YCSB benchmarks.


To run YCSB on MongoDB with UNVMe device:

    $ cd /WORK/mongo
    $ unvme-setup
    07:00.0 Intel Corporation DC P3700 SSD [2.5" SFF] - (enabled for UNVMe)
    $ /WORK/unfs/wiredtiger/unfs-mongo-ycsb 07:00.0


To run YCSB on MongoDB with raw device:

    $ cd /WORK/mongo
    $ unvme-setup reset
    $ /WORK/unfs/wiredtiger/unfs-mongo-ycsb /dev/nvme0n1


To run YCSB on MongoDB with native XFS filesystem:

    $ cd /WORK/mongo
    $ mkfs -t xfs /dev/nvme0n1
    $ mount /dev/nvme0n1 /data
    $ /WORK/unfs/wiredtiger/unfs-mongo-ycsb


By default, unfs-mongo-ycsb script will load and test 1,000,000 records
with 1, 8, 16, and 32 threads using the mongodb-async driver.

It should be noted that UNFS/UNVMe stack is primarily designed to
demonstrate a paradigm shift for future 3D-Crosspoint SSD products and
may not perform well with current NAND based SSD technology.

