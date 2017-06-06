#!/bin/bash
#remove fifo's to permit simulation
for n in `seq 0 9`; do
	f="/tmp/rtf$n" ;
	if test  -p $f ; then
		sudo rm $f;
		fi ;
	done;
