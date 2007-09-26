#!/bin/bash
if [[ $# -ne 1 ]] ; then
	echo "$0 <path-to-ofa-kernel-tree>"
	echo "eg: $0 /usr/src/ofa_kernel-1.2.5"
	exit 1
fi;

echo "Copying krping files to $1/drivers/infiniband/hw/cxgb3"
cp -f krping.c getopt.[ch] $1/drivers/infiniband/hw/cxgb3
if [[ $? -ne 0 ]] ; then
	echo "cp failed!"
	exit 1
fi

echo "Patching $1/drivers/infiniband/hw/cxgb3/Makefile"
patch -d $1 -p1 < krping-ofa.patch
if [[ $? -ne 0 ]] ; then
	echo "path apply failed!"
	exit 1
fi

echo "Done! "
echo "Now configure, build, and install the ofa kernel tree to get the rdma_krping module installed."

exit 0
