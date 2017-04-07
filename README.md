UNFS - User Space Nameless Filesystem
=====================================

UNFS is a user space filesystem developed at Miron Technology.
It is designed as a proof of concept to enable applications that
have dependency on filesystem to work with UNVMe driver.

UNFS is implemented as a library with APIs and a set of commands to format,
check, and examine the filesystem.

UNFS can be run with the user-space UNVMe driver (i.e. for NVMe based
devices) or against any raw block device (i.e. direct I/O) supported by
a kernel space driver.

This project also contains a plugin implementation that enabled MongoDB
to run on UNFS with UNVMe driver.  The latest stable MongoDB 3.4.3 release
has (unofficial) support for custom filesystem through its WiredTiger data
engine where UNFS is plugged with.



System Requirements
===================

UNFS is implemented and verified on Linux CentOS 7 running on x86_64 based
systems.  Note that root privilege is required to access the raw device.

Although UNFS can be configured to be standalone, for completeness,
additional source packages may be required to demonstrate its full operation. 

Two main supported packages are UNVMe driver and MongoDB 3.4.3 (and later).
Each of these packages may have its own requirements.  For example,
UNVMe requires x86 system with VT-d support, and MongoDB requires at least
GCC version 5.3 (which must be installed and compiled on CentOS 7), etc.
YCSB package is also required for running DB benchmarks.



Build and Test UNFS with UNVMe Driver
=====================================

Assuming /opt is the base working directory for various packages.

First, download and install the UNVMe driver:

    $ cd /opt
    $ git clone https://github.com/MicronSSD/unvme.git
    $ cd /opt/unvme
    $ make install


Then download and install UNFS:

    $ cd /opt
    $ git clone https://github.com/MicronSSD/unfs.git
    $ cd /opt/unfs
    $ make install


To setup UNVMe and run the UNFS unit tests:

    $ unvme-setup bind
    0a:00.0 Dell Express Flash NVMe XS1715 SSD 800GB - (UNVMe enabled)

    $ test/unfs_rmw_test 0a:00.0
    $ test/unfs_tree_test 0a:00.0


UNFS can also be tested using direct I/O mode with the NVMe kernel space driver:

    $ unvme-setup reset
    0a:00.0 Dell Express Flash NVMe XS1715 SSD 800GB - (/dev/nvme0)

    $ test/unfs_rmw_test /dev/nvme0n1
    $ test/unfs_tree_test /dev/nvme0n1


There are also utility programs namely unfs_format, unfs_check, and
unfs_shell.  They can be used to format a device, verify the filesystem
integrity, and invoke commands through the provided shell program to
interactively browse files and directories respectively.

Note that when using the UNVMe driver each test program will take longer
to start because the driver will first be loaded and initialized each time
before the actual application or test can run.



Build and Test MongoDB on UNFS
==============================

MongoDB custom filesystem support requires the source code to be specially
compiled.  Use the following procedure to download and compile MongoDB:

    $ cd /opt
    $ git clone https://github.com/mongodb/mongo.git -b v3.4

    $ cd /opt/mongo/src/third_party/wiredtiger
    $ ./autogen.sh
    $ ./configure --with-builtins=snappy,zlib
    $ make -j16

    $ cd /opt/mongo
    $ scons CPPPATH=/opt/mongo/src/third_party/wiredtiger \
            LIBPATH=/opt/mongo/src/third_party/wiredtiger/.libs \
            RPATH=/opt/mongo/src/third_party/wiredtiger/.libs \
            --use-system-wiredtiger -j16 all


The UNFS plugin for MongoDB also has to be built after the MongoDB source is
built, by specifying where the MongoDB source is:

    $ cd /opt/unfs

    $ MONGODIR=/opt/mongo make install
    (Or edit Makefile.def to set MONGODIR=/opt/mongo)


To verify the UNFS plugin for MongoDB, run:

    $ unvme-setup bind
    0a:00.0 Dell Express Flash NVMe XS1715 SSD 800GB - (UNVMe enabled)

    $ mongo/unfs_wt_test 0a:00.0


Now the MongoDB service can be launched using the provided configuration file:

    $ mkdir -p /data/db

    $ unfs_format 0a:00.0
    (Run unfs_format is equivalent to mkfs on a new device)

    $ UNFS_DEVICE=0a:00.0 /opt/mongo/mongod --quiet --config /opt/unfs/mongo/unfs-mongo.conf
    

And then use the client to access the database interactively:

    $ /opt/mongo/mongo

Or running a jstest:

    $ /opt/mongo/mongo /opt/mongo/jstests/core/all.js


Note that technically when using UNFS, there should not be any dependency
on native filesystem.  However, MongoDB does not yet officially support
custom filesystem feature, and it still requires the data directory
(e.g. /data/db) to be created on the native filesystem where it stores
certain diagnostic data (i.e. less than 1MB).

Also beware that UNFS-UNVMe only supports single application instance per
device, i.e. only one application can access one device at any one time.



Run MongoDB Performance Test
============================

To run mongoperf against UNFS using UNVMe driver:

    $ unvme-setup bind
    $ rm -rf /data/db
    $ mkdir -p /data/db
    $ unfs_format 0a:00.0
    $ UNFS_DEVICE=0a:00.0 /opt/mongo/mongod --quiet --config /opt/unfs/mongo/unfs-mongo.conf

    In another window run:

    $ echo "{nThreads:8,fileSizeMB:1024,r:true,w:true}" | /opt/mongo/mongoperf


To run mongoperf against UNFS using direct I/O:

    $ unvme-setup reset
    $ rm -rf /data/db
    $ mkdir -p /data/db
    $ unfs_format /dev/nvme0n1
    $ UNFS_DEVICE=/dev/nvme0n1 /opt/mongo/mongod --quiet --config /opt/unfs/mongo/unfs-mongo.conf

    In another window run:

    $ echo "{nThreads:8,fileSizeMB:1024,r:true,w:true}" | /opt/mongo/mongoperf


To run mongoperf against native XFS:

    $ unvme-setup reset
    $ mkdir -p /data/db
    $ mkfs.xfs -f /dev/nvme0n1
    $ mount -o dirsync,sync /dev/nvme0n1 /data/db
    $ /opt/mongo/mongod --quiet

    In another window run:

    $ echo "{nThreads:8,fileSizeMB:1024,r:true,w:true}" | /opt/mongo/mongoperf


Note that native XFS is mounted with the sync option to disable cache
for a better comparison against the cacheless UNFS.



Run YCSB Benchmarks
===================

A shell script (i.e. /opt/unfs/mongo/unfs-mongo-ycsb) is provided to run
YCSB benchmarks.  YCSB 0.12.0 (from https://github.com/brianfrankcooper/YCSB) 
is assumed to have already been installed on the system in /opt/ycsb.

By default, the benchmark script should be run from the MongoDB source
directory in which it will load and perform read-write of 1,000,000 records
using 1, 8, and 16 threads.  The benchmark results will be stored in the
/data/ycsb-results directory.  The script will automatically kill and
launch mongod as needed.


To run YCSB benchmarks against UNFS using UNVMe driver:

    $ unvme-setup bind
    $ mkdir -p /data/db
    $ /opt/unfs/mongo/unfs-mongo-ycsb 0a:00.0


To run YCSB benchmarks against UNFS using direct I/O:

    $ unvme-setup reset
    $ mkdir -p /data/db
    $ /opt/unfs/mongo/unfs-mongo-ycsb /dev/nvme0n1


To run YCSB benchmarks against native XFS:

    $ unvme-setup reset
    $ mkdir -p /data/db
    $ mkfs.xfs -f /dev/nvme0n1
    $ mount -o dirsync,sync /dev/nvme0n1 /data/db
    $ /opt/unfs/mongo/unfs-mongo-ycsb
    $ umount /data/db


It should be noted that the user space UNFS-UNVMe stack is primarily designed
for future 3D XPoint products to demonstrate performance advantage of the
user polling model for very fast device (i.e. less than 5 microseconds
response time).  UNFS-UNVMe stack may not perform as well as native filesystem
running on slower NAND-based SSD due to more CPU cycles being consumed by
polling mode (vs interrupt handling).  UNFS is a cacheless filesystem and
should be compared to native filesystem mounting with cache disabled.
The performance gap between cache and cacheless filesystem should close up
with 3D XPoint products.

