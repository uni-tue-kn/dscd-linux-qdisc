#!/bin/bash

set -o errexit
set -o nounset
shopt -s nullglob

sched_dir="../dscd_scheduler/"
dist_dir="tc_lib"


if [[ "$#" -eq 1 && "$1" == "clean" ]]; then
    cd iproute2
    make clean
	cd ..
	rm -v iproute2/tc/q_dscd.c || true
	for f in ${sched_dir}/include/uapi/linux/*; do
		echo $f
		rm -v iproute2/include/uapi/linux/$(basename $f) || true
	done
	rm -rv $dist_dir || true
	exit 0
fi


# copy tc module source into tc tree
cp q_dscd.c iproute2/tc/

# copy uapi header files
cp ${sched_dir}/include/uapi/linux/* iproute2/include/uapi/linux/

( # compile tc
    cd iproute2
    make -j3 TCSO=q_dscd.so
)

# copy dscd tc module
mkdir $dist_dir || true
cp iproute2/tc/q_dscd.so $dist_dir

exit 0
