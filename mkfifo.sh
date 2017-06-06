#create fifo's if not already there
for n in `seq 0 9`; do
	f="/tmp/rtf$n" ;
	if test ! -p $f ; then
		sudo mkfifo -m 666 $f;
		fi ;
	done;
