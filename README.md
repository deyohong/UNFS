UNFS - User Space Nameless Filesystem
=====================================

UNFS is a user space simple filesystem developed at Miron Technology.
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

UNFS is implemented and verified on Linux CentOS 6 and 7 running on x86_64
based systems.  Note that root privilege is required to access raw device.

Although UNFS can be configured to be standalone, for completeness,
it requires additional source packages to be downloaded including UNVMe
driver, WiredTiger with custom filesystem feature support, and MongoDB.
Each of these packages may have its own requirements.  For example,
UNVMe requires Linux with a kernel version that supports VFIO, and
MongoDB requires at least GCC version 5.3 (which does not come with
CentOS 6 or 7), etc.



Build UNFS & MongoDB
====================

The latest MongoDB 3.4 release does not yet support plugin custom filesystem.
So to build and run MongodDB on UNFS, first download and build the WiredTiger
and MongoDB source along with UNVMe and UNFS.  Assume the working directory 
is /opt, use the following procedure (tested on CentOS):

    1) Download and build WiredTiger 'develop' branch code:

        $ cd /opt
        $ git clone https://github.com/wiredtiger/wiredtiger.git -b develop
        $ cd wiredtiger
        $ ./autogen.sh
        $ ./configure --with-builtins=snappy,zlib
        $ make -j16


    2) Download and build MongoDB to use the custom WiredTiger engine:

        $ cd /opt
        $ git clone https://github.com/mongodb/mongo.git -b v3.4
        $ cd mongo
        $ scons CPPPATH=/opt/wiredtiger LIBPATH=/opt/wiredtiger/.libs RPATH=/opt/wiredtiger/.libs --use-system-wiredtiger -j16


    3) Download and install the UNVMe driver:

        $ cd /opt
        $ git clone https://github.com/MicronSSD/unvme.git
        $ cd unvme
        $ make install


    4) Download and install UNFS:

        $ cd /opt
        $ git clone https://github.com/MicronSSD/unfs.git
        $ cd unfs
          # then edit Makefile.def and set the WIREDTIGERDIR variable
          # to build the plugin library libunfswt.so for MongoDB
        $ make install


Note that "make install" will install the required header files and libraries
in /usr/local.  To put into effect, make sure /usr/local/bin is set in the
PATH environment variable, and that ld.so.conf contains /usr/local/lib entry.

    $ cat /etc/ld.so.conf
    include ld.so.conf.d/*.conf
    /usr/local/lib64
    /usr/local/lib

    $ ldconfig



Run MongoDB Tests
=================

Assuming the custom MongoDB filesystem is an NVMe device, the device must
first be UNFS formatted as follows:

    $ /usr/local/bin/unvme-setup
    07:00.0 Dell Express Flash NVMe XS1715 SSD 800GB - (enabled for UNVMe)

    $ export UNFS_DEVICE=07:00.0

    $ /usr/local/bin/unfs_format
    UNFS format device 07:00.0 label "User Space Nameless Filesystem"


Note that running unfs_format is equivalent to invoking mkfs on a new device.
For raw device testing, there is no need to run unvme-setup, and UNFS_DEVICE
should be set to the actual device name instead.


To run MongoDB:

    $ /opt/mongo/mongod --config /opt/unfs/wiredtiger/unfs-mongo.conf



Run YCSB Benchmarks
===================

Assume YCSB 0.12.0 is already installed on the system (in /opt/ycsb),
a shell script wiredtiger/unfs-mongo-ycsb is provided for YCSB benchmarks.
The script, by default, will load and test 1,000,000 records using 1, 8, and
16 threads.  These values can be changed in the script or on the command line.


To setup for UNVMe device before running YCSB:

    $ /usr/local/bin/unvme-setup
    07:00.0 Dell Express Flash NVMe XS1715 SSD 800GB - (enabled for UNVMe)

    $ export UNFS_DEVICE=07:00.0


To setup for raw device before running YCSB:

    $ /usr/local/bin/unvme-setup reset
    07:00.0 Dell Express Flash NVMe XS1715 SSD 800GB - (mapped to nvme0)

    $ export UNFS_DEVICE=/dev/nvme0n1


To setup for native XFS filesystem on NVMe device:

    $ mkfs -t xfs /dev/nvme0n1

    $ mount /dev/nvme0n1 /data

    $ unset UNFS_DEVICE


To run YCSB on MongoDB:

    $ cd /opt/mongo
    $ /opt/unfs/wiredtiger/unfs-mongo-ycsb


To override the number of operations and threads:

    $ OPCOUNT=50000 THREADS='1 4' /opt/unfs/wiredtiger/unfs-mongo-ycsb


It should be noted that the user space UNFS/UNVMe stack is primarily designed
for future 3D Xpoint products (to demonstrate user polling performance
advantage for fast device with less than 5 usec response time).  UNFS/UNVMe,
however, does not perform well with slower NAND-based SSD technology due to
synchronous polling.

