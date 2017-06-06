#!/bin/bash
if [ "k$1" == "k" ] ; then
	sudo taskset 01 ./ytest -f -p-20
#	sudo taskset 0x08 ./ytest -f -p-20
	else
	sudo taskset 01 ./ytest -f -p-20 -n $1
#	sudo taskset 0x08 ./ytest -f -p-20 -n $1
fi
