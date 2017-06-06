/*
COPYRIGHT (C) 2001-2014  Giuliano Colla - Copeca srl (colla@copeca.it)

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
*/

#define USE_FIFO

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
// for get/setpriority
#include <sys/resource.h>
#ifdef USE_FIFO
#include <fcntl.h>
#include <unistd.h>
#endif

#include "ycore.h"
#ifdef LSCHED
#define rqactv active_task()
#endif
  
#define start	0x40
#define stop	0x41
#define begin	0x42

#ifdef USE_FIFO
#define FIFO0	"/tmp/rtf0"
#define FIFO1	"/tmp/rtf1"
#define FIFO2	"/tmp/rtf2"
#define FIFO3	"/tmp/rtf3"
#define FIFO4	"/tmp/rtf4"
#define	BUFSIZE	1024
#endif

#define SYSTEM_CLOCK CLOCK_REALTIME
//#define SYSTEM_CLOCK CLOCK_MONOTONIC

TASK_DESCRIPTOR frankTd,sinatraTd,watchdogTd;

void frank(void);
void sinatra(void);
void watchdog(void);

STATIC_TASK_DESCRIPTOR frankStd = STD_INITIALIZER("FRANK",frank,145,frankTd);
STATIC_TASK_DESCRIPTOR sinatraStd = STD_INITIALIZER("SINATR",sinatra,150,sinatraTd);
STATIC_TASK_DESCRIPTOR watchdogStd = STD_INITIALIZER("WATCHD",watchdog,200,watchdogTd);



/*{	"FRANK*",
	&frank,
	NULL,
	16384,
	0,
	145,
	NULL,
	&frankTd,
	0};

STATIC_TASK_DESCRIPTOR sinatraStd = {
	"SINATR",
	&sinatra,
	NULL,
	16384,
	0,
	150,
	NULL,
	&sinatraTd,
	0};

STATIC_TASK_DESCRIPTOR watchdogStd = {
	"WATCHD",
	&watchdog,
	NULL,
	16384,
	0,
	200,
	NULL,
	&watchdogTd,
	0};
*/
EXCHANGE_DESCRIPTOR frank_exch = EXCH_INITIALIZER(" FRANK");
EXCHANGE_DESCRIPTOR sinatra_exch = EXCH_INITIALIZER("SINATR");


/* {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	PTHREAD_MUTEX_INITIALIZER,
	" FRANK"
	};
EXCHANGE_DESCRIPTOR sinatra_exch ={
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	PTHREAD_MUTEX_INITIALIZER,
	"SINATR"
	};
*/

STD_LIST_PTR task_table[] = {
	&frankStd,
	&sinatraStd,
	&watchdogStd,
	NULL
	};

//EXCHANGE_LIST_PTR exch_table[MAX_EXCH] = {
EXCHANGE_LIST_PTR exch_table[] = {
	&frank_exch,
	&sinatra_exch,
	NULL
	};

MSG_DESCRIPTOR a_message;
static int cycles,max_cycles;
static int done = 0;
static int kwait = 0;

#define PROFILE
#ifdef PROFILE
struct timespec rt_start_time,mt_start_time;
struct timespec rt_stop_time,mt_stop_time;
struct timespec skew_time;
struct timespec timer_res;
int tdiff;
#endif

void frank(void) {
	MSG_DESCRIPTOR *msg,*no_msg;
	EXCHANGE_DESCRIPTOR wait_exch;
#ifdef PROFILE
	struct timespec CurrTime,PrevTime;
	long Curr_ns,Prev_ns,Min_ns,Max_ns,Avg_ns;
	long long Tot_ns;
	int CountBig,WhichBig;
#endif	
#ifdef USE_FIFO
	int fd,count;
	unsigned char buf[BUFSIZE];
#endif

	rqcxch(&wait_exch,"*WAIT*\0");
	//rqelvl(TIMER_8254_IRQ);
#ifdef PROFILE
	Min_ns = 1000000000L;
	Max_ns = Tot_ns = CountBig = WhichBig = 0;
#endif	

#ifdef USE_FIFO
	if ((fd = open(FIFO0, O_WRONLY | O_NONBLOCK)) < 0) {
		fprintf(stderr, "Error opening %s\n",FIFO0);
		exit(1);
	}

#endif
	if (max_cycles > 100000) rqwait(&wait_exch,100); /* let start wdog */
	printf("Tstart = %d\n",rqsystime);
	//sleep(1);
	msg = &a_message;
	rqcmsg(&a_message, sizeof a_message);

	msg->type = begin;
	msg->response_exchange = &frank_exch;
	rqsend(&sinatra_exch,msg);
	msg = rqwait(&frank_exch,0);

	cycles = 0;
	forever {
		if (max_cycles < 1000) {
#ifdef USE_FIFO
			count = sprintf(buf,"\x1b[31m%d Frank\x1b[39;49m",cycles);
			count = write(fd,buf,count);
#else
			printf("\x1b[31m%d Frank\x1b[39;49m",cycles);
#endif			
			}
		msg->type = start;
		msg->response_exchange = &frank_exch;
#ifdef PROFILE
		clock_gettime(SYSTEM_CLOCK,&CurrTime);
		msg->yltime = CurrTime.tv_nsec;
#endif
		rqsend(&sinatra_exch,msg);
		msg = rqwait(&frank_exch,0);
#ifdef PROFILE
		clock_gettime(SYSTEM_CLOCK,&CurrTime);
		Curr_ns = CurrTime.tv_nsec;
		Prev_ns = msg->yltime;
		Curr_ns = Curr_ns - Prev_ns;
		if (Curr_ns < 0 ) Curr_ns += 1000000000L;
		if (Curr_ns > Max_ns) Max_ns = Curr_ns;
		if (Curr_ns < Min_ns) Min_ns = Curr_ns;
		if (Curr_ns > 1000000L) {
			CountBig++;
			WhichBig = cycles;
			}
		Tot_ns += Curr_ns;
#endif
		if (++cycles > max_cycles) {
#ifdef USE_FIFO
			count = sprintf(buf,"\x1b[31mwas The Voice\x1b[39;49m\n");
			count = write(fd,buf,count);
			count = sprintf(buf,"Tstop = %d\n",rqsystime);
			count = write(fd,buf,count);
#else
			printf("\x1b[31mwas The Voice\x1b[39;49m\n");
			printf("Tstop = %d\n",rqsystime);
#endif
			msg->type = 0x41;
			msg->response_exchange = &frank_exch;
			rqsend(&sinatra_exch,msg);
			msg = rqwait(&frank_exch,0);
#ifdef PROFILE
#ifdef USE_FIFO
			count = sprintf(buf,"Big = %d @ %d - Round_trip Min=%ld ns, Max=%ld ns, Avg=%ld ns\n",
				CountBig,WhichBig,Min_ns,Max_ns,Tot_ns/cycles);
			count = write(fd,buf,count);
#else
			printf("Round_trip Min=%ld ns, Max=%ld ns, Avg=%ld ns\n",Min_ns,
				Max_ns,Tot_ns/cycles);
#endif
#endif
#ifdef USE_FIFO
			close(fd);
#endif	
			rqdtsk(&sinatraTd);
			/*if (max_cycles <= 100000) */ rqdtsk(&watchdogTd);
			//rqdxch(&frank_exch);
			//rqdxch(&sinatra_exch);
			rqdtsk(rqactv);
			//rqfreeze();
			}
		//if (cycles % 100 == 0) printf(".");
		//sleep(1);
		if (max_cycles < 1000) 
			//no_msg = rqwait(&frank_exch,50);
			no_msg = rqwait(&wait_exch,1);
		else /* if (max_cycles <= 100000) */ no_msg = rqwait(&wait_exch,1);
 		}
	}

#define TMAX 0

void sinatra(void) {

	MSG_DESCRIPTOR *msg;
	int tmax,i;
#ifdef PROFILE
	struct timespec CurrTime,PrevTime;
	long Curr_ns,Prev_ns,Min_ns,Max_ns,Avg_ns;
	long long Tot_ns;
	int s_cycles;
#endif	

#ifdef USE_FIFO
	int fd,count;
	unsigned char buf[BUFSIZE];
#endif

	tmax = TMAX;
	i= 0;
#ifdef PROFILE
	Min_ns = 1000000000L;
	Max_ns = Tot_ns = 0;
	s_cycles = 0;
#endif	
#ifdef USE_FIFO
	if ((fd = open(FIFO0, O_WRONLY | O_NONBLOCK)) < 0) {
		fprintf(stderr, "Error opening %s\n",FIFO0);
		exit(1);
	}

#endif
	forever {
		msg = rqwait(&sinatra_exch,tmax);
//		rt_printk(" Sinatra %d %d %d\n",tmax,msg->type,i);
		if (msg->type > timeout_type) {
			if (msg->type == start) {
#ifdef PROFILE
				s_cycles++;
				clock_gettime(SYSTEM_CLOCK,&CurrTime);
				Curr_ns = CurrTime.tv_nsec;
				Prev_ns = msg->yltime;
				Curr_ns = Curr_ns - Prev_ns;
				if (Curr_ns < 0 ) Curr_ns += 1000000000L;
				if (Curr_ns > Max_ns) Max_ns = Curr_ns;
				if (Curr_ns < Min_ns) Min_ns = Curr_ns;
				Tot_ns += Curr_ns;		
#endif
				if (max_cycles < 1000) {
#ifdef USE_FIFO
					count = sprintf(buf,"\x1b[31m Sinatra\x1b[39;49m\n");
					count = write(fd,buf,count);
#else
					printf("\x1b[31m Sinatra\x1b[39;49m\n");
#endif
					}			
				i = 0;
				tmax = TMAX;
				rqsend(msg->response_exchange,msg);
				}
			else if (msg->type == begin) {
				rqsend(msg->response_exchange,msg);
				}  
			else {
#ifdef PROFILE
#ifdef USE_FIFO
				count = sprintf(buf,"One-way Min=%ld ns, Max=%ld ns, Avg=%ld ns\n",Min_ns,
					Max_ns,Tot_ns/s_cycles);
				count = write(fd,buf,count);
#else
				printf("One-way Min=%ld ns, Max=%ld ns, Avg=%ld ns\n",Min_ns,
					Max_ns,Tot_ns/s_cycles);
#endif
#endif
				rqsend(msg->response_exchange,msg);
#ifdef USE_FIFO
				close(fd);
#endif	
				rqdtsk(rqactv);
				//rqsusp(&sinatraTd);
				}
			} 
		else {
			if (++i > 5) {
				tmax = 0;
				printf("- %d- ",i);
				}
			}
		}
	}

void watchdog(void) {
	int w_cycles;
	EXCHANGE_DESCRIPTOR wd_exch;
	w_cycles = 0;
	rqcxch(&wd_exch,"WATCHD\0");
	forever {
		/* use prime numbers to randomize a bit */
		if (max_cycles <= 100000) rqwait(&wd_exch,127);
		else rqwait(&wd_exch,1117); 
		w_cycles++;
		if ((w_cycles % 10) == 0) printf("Arf\n");
		}
	}

int main(int argc,char *argv[]) {
	int i;
	int policy = SCHED_OTHER;
	int priority;
	int req_priority = 0;
 	struct sched_param mysched;
#ifdef USE_FIFO
	int fdc,fda,fdw,fdr; /* handles for fifo's */
	int count;
	unsigned char buf[BUFSIZE];
	unsigned char c;
	int is_running;

	unsigned char WaitKey (unsigned int millisec) {
		fd_set set;
		struct timeval timeout;
		int sel_val,c;
		while (is_running) {
			FD_ZERO(&set);
			FD_SET(STDIN_FILENO, &set);
			FD_SET(fdc, &set);
			FD_SET(fdw, &set);
			timeout.tv_sec = millisec/1000;
			timeout.tv_usec = (millisec % 1000)*1000;
			sel_val =select (FD_SETSIZE,
						&set,NULL,NULL,
						&timeout);
			if (sel_val) {
				kwait = 0;
				if (FD_ISSET(fdc,&set)) {
					count = read(fdc,buf,BUFSIZE);
					write(STDOUT_FILENO,buf,count);
					}
				if (FD_ISSET(fdw,&set)) {
					count = read(fdc,buf,BUFSIZE);
					write(STDOUT_FILENO,buf,count);
					}
				if (FD_ISSET(STDIN_FILENO, &set)) {
					c = getchar();
					return c;
					}
				}
			else if (done) {
				if (++kwait > 50) {
					count = sprintf (buf,"Enter 'q' to terminate\n");
					write(STDOUT_FILENO,buf,count);
					kwait = 0;
					}
				}
			if (cycles >= max_cycles && !done) done = true;
			}
		}

	is_running = true;
#endif

	max_cycles = 10000;
	while (argc > 1 && argv[1][0] == '-' ) {
		switch (argv[1][1]) {
		case 'n':  /* -n num_cycles */
			max_cycles = atoi(&argv[1][2]);
			break;
		case 'f':  /* -f fifo */
			policy = SCHED_FIFO;
			break;
		case 'o':  /* -o other */
			policy = SCHED_OTHER;
			break;
		case 'p':  /* -p priority */
			req_priority = atoi(&argv[1][2]);
			break;
		case 'r':  /* -r RoundRobbin */
			policy = SCHED_RR;
			break;
		default:
			fprintf(stderr, "%s: unknown arg %s\n",argv[0],argv[1]);
			exit(1);
		}
		argc--;
		argv++;
	}

    if (policy != SCHED_OTHER) {
		mysched.sched_priority = 99;
		if( sched_setscheduler( 0, policy, &mysched ) == -1 ) {
			puts(" ERROR IN SETTING THE SCHEDULER UP");
			perror( "errno" );
			exit( 0 );
 			}
		}
	printf("Scheduling policy set to ");
	switch (policy) {
		case SCHED_OTHER:
			puts("SCHED_OTHER");
			break;
		case SCHED_FIFO:
			puts("SCHED_FIFO");
			break;
		case SCHED_RR:
			puts("SCHED_RR");
			break;
		default:
			puts("UNKNOWN");
			break;
		}
	
	//max_cycles = 100000;
	if (argc > 1) max_cycles = atoi(argv[1]);
	priority = getpriority(0,0);
	if (priority != req_priority) {
		printf("Setting priority to %d\n",req_priority);
		if (setpriority(0,0,req_priority) < 0 ) {
			puts(" ERROR IN SETTING UP PRIORITY");
			perror( "errno" );
			exit( 0 );
			}
		priority = getpriority(0,0);
		}

#ifdef USE_FIFO
	fprintf(stderr, "Opening %s\n",FIFO0);
//	if ((fdc = open(FIFO0, O_WRONLY | O_NONBLOCK)) < 0) {
	if ((fdc = open(FIFO0, O_RDWR | O_NONBLOCK)) < 0) {
		fprintf(stderr, "Error opening %s\n",FIFO0);
		exit(1);
	}

	fprintf(stderr, "Opening %s\n",FIFO2);
//	if ((fdw = open(FIFO2, O_WRONLY | O_NONBLOCK)) < 0) {
	if ((fdw = open(FIFO2, O_RDWR | O_NONBLOCK)) < 0) {
		fprintf(stderr, "Error opening %s\n",FIFO2);
		exit(1);
	}
#endif
	printf("Starting rqstart: priority=%d\n",priority);

#ifdef VERBOSE
	printf("frank_ex=%p\n",&frank_exch);
	//printf("sinatra_ex=%p\n",&sinatra_exch);
	printf("exch_table = %p\n",&exch_table);
	for (i=0;i < 3; i++) {
		printf ("exch_table[%d] = %p\n",i,exch_table[i]);
		}
#endif

#ifdef PROFILE
	clock_gettime(CLOCK_REALTIME,&rt_start_time);
	clock_gettime(CLOCK_MONOTONIC,&mt_start_time);
#endif

	rqstart(task_table,3,exch_table,/*exch_names,*/3);
	printf("return from rqstart\n");
	sleep(1);
#ifdef USE_FIFO
	printf("Executing %d cycles\n",max_cycles);
	printf("Enter d,r,x,t,n to debug, q to terminate\n");
	while (is_running) {
		c = WaitKey(300);
		if (c == 'q') is_running = false;
		else if (c == 'd') display_delay_list();
		else if (c == 'x') display_exchange_list();
		else if (c == 'r') display_ready_list();
		else if (c == 't') display_task_list();
		else if (c == 'f') rqfreeze();
		else if (c == 'b') rqbake();
		else if (c == 'n') printf ("Cycles = %d, remaining = %d\n",cycles,max_cycles-cycles);
		else if (c == 's') printf ("String %6.6s\n","A_VERY_LONG_STRING");
		}
#endif
	//rqctsk(&frankStd);
	if (cycles < max_cycles) printf("Aborted!\n");
	max_cycles = cycles;
	printf("waiting Frank\n");
	pthread_join(frankTd.thread,NULL);
	pthread_cancel(sinatraTd.thread);
	printf("waiting Sinatra\n");
	pthread_join(sinatraTd.thread,NULL);
	if (watchdogTd.thread) {
		pthread_cancel(watchdogTd.thread);
		printf("waiting Watchdog\n");
		pthread_join(watchdogTd.thread,NULL);
		}
#ifdef PROFILE
	clock_gettime(CLOCK_REALTIME,&rt_stop_time);
	clock_gettime(CLOCK_MONOTONIC,&mt_stop_time);
	rt_stop_time = tsSubtract(rt_stop_time,rt_start_time);
	mt_stop_time = tsSubtract(mt_stop_time,mt_start_time);
	tdiff = tsCompare(rt_stop_time,mt_stop_time);
	switch (tdiff) {
		case 0: printf("No time skew REALTIME / MONOTONIC\n");
			break;
		case 1: skew_time = tsSubtract(rt_stop_time,mt_stop_time);
			printf ("Time skew MONOTONIC>REALTIME by %ld ns\n",skew_time.tv_nsec);
			break;
		case -1: skew_time = tsSubtract(mt_stop_time,rt_stop_time);
			printf ("Time skew MONOTONIC<REALTIME by %ld ns\n",skew_time.tv_nsec);
			break;
		}
	clock_getres(CLOCK_REALTIME,&timer_res);
	printf("CLOCK_REALTIME resolution = %ld ns\n",timer_res.tv_nsec);
	clock_getres(CLOCK_MONOTONIC,&timer_res);
	printf("CLOCK_MONOTONIC resolution = %ld ns\n",timer_res.tv_nsec);
	
#endif

	exit(0);
	}
