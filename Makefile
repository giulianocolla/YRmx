CFLAGS	+=-pthread -g
LDFLAGS	+=-lrt
LDFLAGS	+=-lm

ytest: ycore.o ytest.o
	cc ${CFLAGS} ${LDFLAGS} -o ytest ytest.o ycore.o

ycore.o: ycore.c ycore.h

ytest.o: ytest.c ycore.c ycore.h	

lib: libycore.so ycore.o
CFLAGS +=-fPIC
CFLAGS +=-O3
libycore.so: ycore.o
	cc ${CFLAGS} ${LDFLAGS} -shared $^ -o $@
#	ar -crs libycore.so ycore.o

suid:
	sudo chown root:root ytest ; sudo chmod +s ytest
clean:
	sudo rm ytest ytest.o ycore.o *~ *.bak
