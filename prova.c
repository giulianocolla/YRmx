#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <string.h>

typedef struct {
	int a;
	int b;
	} STR;

typedef STR *STRP;

STR array[10] = {
	{1,1},
	{2,2},
	{3,3},
	{4,4},
	{5,5},
	{6,6},
	{7,7},
	{8,8},
	{9,9},
	{10,10}
	};

STR array1[10] = {
	{11,11},
	{12,12},
	{13,13},
	{14,14},
	{15,15},
	{16,16},
	{17,17},
	{18,18},
	{19,19},
	{20,20}
	};

STRP parray[3] = {
	&array[0],
	&array1[0],
	NULL
	};

void subr(STRP param[]) {
	int i;

	for (i=0;i < 3; i++) {
		printf ("param[%d] = %p\n",i,param[i]);
		}
	}

int main(int argc,char *argv[]) {
	int i;


	printf ("parray = %p\n",&parray);
	for (i=0;i < 3; i++) {
		printf ("parray[%d] = %p\n",i,parray[i]);
		}
	subr(parray);
	exit(0);
	}
