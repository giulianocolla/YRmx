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

/* ycore is an emulation on linux envirnomnent of the Intel iRMX88
nanokernel, providing the same features, but taking advantage of linux kernel
functions. As opposed to YALRT, it runs in user space */

/*

GENERAL CHOICE
In order to reserve one CPU for real-time, and avoid the cache invalidation
overhead we can either use cpuset at system level, or sched_affinity locally 

*/

#define USE_CPUSET
#ifndef USE_CPUSET
#define USE_AFFINITY
//#undef USE_AFFINITY
#endif

#define MSG_ERR 1
#define EXCH_ERR 2
#define INT_ERR 3
#define MSG_NULL_ERR 4

#include <stdio.h>
#include <stdlib.h>
#ifdef USE_AFFINITY
#define __USE_GNU
#include <sched.h>
#endif
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <string.h>

//#define PROFILE_TIMER
#ifdef PROFILE_TIMER
#include <math.h>
#endif

#include "ycore.h"
// Support for logging to file
#include <stdarg.h>
#include <time.h>

time_t t;
struct tm tm;


int no_print (const char *fmt, ...) {
  return (0);
    }

#define printinfo printlog

FILE *logfile = 0;

int printlog (const char *fmt, ...) {
	va_list argptr;
	if (logfile) {
  		va_start(argptr,fmt);
  		vfprintf(logfile,fmt,argptr);
  		va_end(argptr);
  		}
	va_start(argptr,fmt);
	vprintf(fmt,argptr);
 	va_end(argptr);
	}

void printdate (void) {
	if (logfile) {
		t = time(NULL);
		fprintf(logfile,ctime(&t));
		}
	//tm = *localtime(&t); // oppure time()
	//printinfo("now: %d/%d/%d %d:%d:%d\n", tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
	}


#define NEVER 0xFFFF
struct timespec tsNEVER = {365,0}; // One year!
#define SYSTEM_CLOCK CLOCK_REALTIME
//#define SYSTEM_CLOCK CLOCK_MONOTONIC
#define SEM_CLOCK CLOCK_REALTIME

// Debug Options
//#define BY_STEP
//#define VERBOSE
#ifdef VERBOSE
#define printk printinfo
#else
#define printk no_print
#endif

//#define WITH_READY_LIST

void display_ready_list(void);
void display_task_list(void);
void display_delay_list(void);
void display_exchange_list(void);

TASK_LIST_PTR ready = NULL;

STD_LIST_PTR initial_task_table[MAX_TASK];
EXCHANGE_LIST_PTR initial_exchange_table[MAX_EXCH];
//EXCH_NAME initial_exchange_names [MAX_EXCH];

int rqfrozen = 0;
pthread_cond_t	unfreeze = PTHREAD_COND_INITIALIZER;
pthread_mutex_t	fmutex = PTHREAD_MUTEX_INITIALIZER;

int set_prio = false;

struct task_queue_root {
	TASK_LIST_PTR headPtr;
	TASK_LIST_PTR tailPtr;
	};

struct task_queue_root suspend_list_root ={NULL,(void *)&suspend_list_root};

DELAY_LIST_QUEUE rq_delay_list_head,rq_delay_list_tail;
pthread_mutex_t	delay_list_mutex = PTHREAD_MUTEX_INITIALIZER;

//unsigned char queue_index;
int queue_index = 0;

INT_EXCHANGE_LIST_PTR isnd_queue[256];

unsigned int rq_sys_excep_ptr;

int critical_region_flag = 0;
pthread_mutex_t mutex1 = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t	Tmutex = PTHREAD_MUTEX_INITIALIZER;
pthread_barrier_t task_barrier;
int rqstarting = true;

struct timespec StartTime,CurrTime;

struct timespec TimeOrigin;

/* System tick: 1ms */
struct timespec rqtick = {0,1000000};
int timetest;

#ifdef PROFILE_TIMER
struct timespec tnow;
int tmin = 1000000000;
int tmax = -1000000000;
int tsum = 0;
float K,Ex,Ex2,Mean,Var;
int ntick = 0;
#endif

unsigned long systime[32];

struct sys_msg time_out_message = {
		NULL,
		sizeof(time_out_message),
		timeout_type
		};

int max_queue = 0;

EXCHANGE_LIST_PTR rq_exchange_head = NULL;
EXCHANGE_LIST_PTR rq_exchange_tail = (void *)&rq_exchange_head;

TASK_LIST_PTR rq_task_head = NULL;
TASK_LIST_PTR rq_task_tail = (void *)&rq_task_head;

static void rqdelaytask(void);
static void rqtimer (void);
void reqisnd (INT_EXCHANGE_DESCRIPTOR *int_ex);
//pthread_t	idle_thread,delay_thread;
//sem_t	idle_sema,delay_sema;

TASK_DESCRIPTOR RootTaskTd;


STATIC_TASK_DESCRIPTOR RootTaskStd = 
	STD_INITIALIZER("*Root*",reqstart,0,RootTaskTd);

INT_EXCHANGE_DESCRIPTOR rq_delay_exchange = {
	NULL,
	(MSG_DESCRIPTOR *)&rq_delay_exchange,
	NULL,
	(TASK_DESCRIPTOR *)&rq_delay_exchange,
	NULL, // link
	PTHREAD_MUTEX_INITIALIZER, // mutex
	"DELYEX",
	0, // assigned
	0, // irq
	&rqtimer, // handler
	NULL, // link
	int_exchange_length,
	int_type,
	0, // yltime
	0, // level
	0, // qint
	0}; // fill

/*---------------- utility procedures --------------*/

/* Linux priority - convert RMX priority (range 0->255) into
Linux FIFO thread priority (range 99->1) 
We reserve 3 levels for system usage:
ROOT task - RMX pri = 0: Linux pri 99 - highest to manage system and terminate properly
TIMER task - RMX pri = 1: Linux pri 98
DELAY task - RMX pri = 2: Linux pri 97
available user task levels: 96-1
*/

int linux_priority(int priority) {
	int lpri;

	if (priority == 0) lpri = 99; /* ROOT, set highest */
	else if (priority == 1) lpri = 98; /* Timer, set next highest */
	else if (priority <= 128) lpri = 97; /* interrupt related, set next highest */
	else {
		priority -= 128; 
		if (priority >127) priority = 127; /* range 0-127 */		
		lpri = 97 - (priority * 96 / 127); /* range 96-1 */
		}
	return (lpri);
	}

unsigned int Ts2Ms (struct timespec *ts) {
	return ts->tv_sec* 1000 + (ts->tv_nsec / 1000000);
	}

struct timespec Ms2Ts (unsigned int* t) {
	struct timespec ts;
	ts.tv_sec = *t / 1000;
	ts.tv_nsec = (*t % 1000) * 1000000;
	return (ts);
	}
 
struct  timespec  tsAdd (struct timespec  time1,struct timespec time2){
    /* Local variables. */
    struct  timespec  result ;
/* Add the two times together. */
    result.tv_sec = time1.tv_sec + time2.tv_sec ;
    result.tv_nsec = time1.tv_nsec + time2.tv_nsec ;
    if (result.tv_nsec >= 1000000000L) {		/* Carry? */
        result.tv_sec++ ;  result.tv_nsec = result.tv_nsec - 1000000000L ;
    	}
    return (result) ;
    }

struct  timespec  tsSubtract (
        struct  timespec  time1,
        struct  timespec  time2) {

    /* Local variables. */
    struct  timespec  result ;
/* Subtract the second time from the first. */
    if ((time1.tv_sec < time2.tv_sec) ||
        ((time1.tv_sec == time2.tv_sec) &&
         (time1.tv_nsec <= time2.tv_nsec))) {		/* TIME1 <= TIME2? */
        result.tv_sec = result.tv_nsec = 0 ;
    	}
    else {						/* TIME1 > TIME2 */
        result.tv_sec = time1.tv_sec - time2.tv_sec ;
        if (time1.tv_nsec < time2.tv_nsec) {
            result.tv_nsec = time1.tv_nsec + 1000000000L - time2.tv_nsec ;
            result.tv_sec-- ;				/* Borrow a second. */
        	}
        else {
            result.tv_nsec = time1.tv_nsec - time2.tv_nsec ;
        	}
    	}
    return (result) ;
	}

void display_sched_attr(int policy, struct sched_param *param)
{
	printinfo("policy=%s, priority=%d\n",
			(policy == SCHED_FIFO)  ? "SCHED_FIFO" :
			(policy == SCHED_RR)	? "SCHED_RR" :
			(policy == SCHED_OTHER) ? "SCHED_OTHER" :
			"???",
			param->sched_priority);
}

/********************************
 TsCompare returns an integer indicating how the times compare:
	-1  if TIME1 < TIME2
	 0  if TIME1 = TIME2
	+1  if TIME1 > TIME2
*******************************************/


int  tsCompare (struct  timespec  time1,struct  timespec  time2) {
    if (time1.tv_sec < time2.tv_sec)
        return (-1) ;				/* Less than. */
    else if (time1.tv_sec > time2.tv_sec)
        return (1) ;				/* Greater than. */
    else if (time1.tv_nsec < time2.tv_nsec)
        return (-1) ;				/* Less than. */
    else if (time1.tv_nsec > time2.tv_nsec)
        return (1) ;				/* Greater than. */
    else
        return (0) ;				/* Equal. */

}
struct timespec curr_time () {
	struct timespec ts;
	if (clock_gettime(SYSTEM_CLOCK, &ts) == -1)
		handle_error("clock_gettime");
	return (ts);
	}

unsigned int reqsystime(){
	CurrTime = curr_time();
	return (CurrTime.tv_sec - StartTime.tv_sec)* 1000 +
		((CurrTime.tv_nsec - StartTime.tv_nsec) / 1000000);
	}

char *task_name(TASK_LIST_PTR this_td) {
	STATIC_TASK_DESCRIPTOR *this_std;
	//printk("task_name %p\n",this_td);
	if (this_td == NULL) {
		return ("(nil)");
		//display_task_list();
		//rqfreeze();
		}
	else {
		this_std = this_td->nameptr;
		//printk("name is %6.6s\n",this_std->name);
		return this_std->name;
		}
	}

/*-------------End of Utility Procedures -----------*/

TASK_LIST_PTR active_task(void) {
	
	TASK_LIST_PTR this_task,found;
	pthread_t	thread;
	STATIC_TASK_DESCRIPTOR *this_std;

	//printk("Entering active_task\n");
	thread = pthread_self();

	this_task = rq_task_head;
	found = NULL;
	while (this_task != NULL) {
		//printk("this_task = %p\n",this_task);
		if (this_task->thread == thread) {
			//printk("found\n");
			found = this_task;
			break;
			}
		this_task = this_task->tasklink;
		}
	return (found);
	}
	
void user_error(int type, void * which){

	//STATIC_TASK_DESCRIPTOR *this_std;
	MSG_DESCRIPTOR *msg;
	TASK_LIST_PTR rqactv;

	rqactv = active_task();
	//this_std = rqactv->nameptr;
	//printinfo("Task %6.6s ",this_std->name);
	printinfo("Task %6.6s ",task_name(rqactv));
	switch (type) {
	case MSG_ERR:
		printinfo("Message Error %p\n",which);
		if (which) {
			msg = which;
			printinfo("This Task= %6.6s, Msg Type = %X Msg Link= %p\n",task_name(rqactv),msg->type,msg->link);
			}
		break;
	case EXCH_ERR:
		printinfo("Exchange Error %p\n",which);
		break;
	case INT_ERR:
		printinfo("Interrupt Error\n");
		break;
	case MSG_NULL_ERR:
		printinfo("Message Error: NULL pointer \n");
		break;
	default:
		printinfo("Unknown Error\n");
		break;
	}
	printinfo("System status:\n");
	display_ready_list();
	display_task_list();
	display_delay_list();
//	critical_region_flag = 0;
//	reqfreeze();
	return;
	}

/*--------------------------------------------
 reqdqmsg:
 Dequeue message from an exchange and make it
 available to currently active task
 SYSTEM INTERNAL USAGE ONLY
 -------------------------------------------*/

static void reqdqmsg (INT_EXCHANGE_LIST_PTR exch,TASK_LIST_PTR rqactv) {

	MSG_DESCRIPTOR *msg;
	int is_int = false;

	if ( (exch->task_head != NULL) | ((void *)(exch->task_tail) != (void *)exch) ) {
		user_error(EXCH_ERR, exch);
		rqactv->message = NULL;
		return;
		}

	msg = exch->message_head;
	//(struct msg_descriptor *)
	rqactv->message = msg;
	if((void *)msg == (void *)&exch->link) {
		if((exch->length == int_exchange_length) && (exch->type < timeout_type)){
			exch->qint = 0;
			is_int = true;
			}
		}
	exch->message_head = msg->link;
	if (msg->link == NULL) {
		exch->message_tail = (void *)exch;
		}
	if (is_int) {msg->link = (void *)msg;}
	else {msg->link = (void *)rqactv;}
	}

/*--------------------------------------------
 canceldelay:
 Dequeue a task from the double linked delay list
 when the task is made active before the delay expires.
 SYSTEM INTERNAL USAGE ONLY
 -------------------------------------------*/

static void cancel_delay_unsafe (TASK_LIST_PTR task){

	TASK_LIST_PTR linktask;

	linktask = task->next;
	linktask->delay = linktask->delay + task->delay;
	linktask->prev = task->prev;

	linktask = task->prev;
	linktask->next = task->next;

	rq_delay_list_tail.delay = 0xFFFF;
#ifdef STOP_TIMER
	if ( (void *)rq_delay_list_head.next == &rq_delay_list_tail ){
		reqdlvl(TIMER_IRQ);}
#endif
	task->status &= ~delayed;
	}

static void cancel_delay (TASK_LIST_PTR task){

	pthread_mutex_lock(&delay_list_mutex);
	cancel_delay_unsafe(task);
	pthread_mutex_unlock(&delay_list_mutex);
	}

/*--------------------------------------------
 reqdqtsk:
 Dequeue a task from an exchange in case
 of reqwait time-out
 SYSTEM INTERNAL USAGE ONLY
 -------------------------------------------*/

static void reqdqtsk (TASK_LIST_PTR delay_task) {
	TASK_LIST_PTR curr_task;
	EXCHANGE_DESCRIPTOR *task_exch;

	task_exch = delay_task->exchange;
	//pthread_mutex_lock( &task_exch->mutex);
	if (!task_exch->task_head) {
		printinfo("rqdqtsk: No Task here!\n");
		rqfreeze();
		//pthread_mutex_unlock( &task_exch->mutex);
		return;
		}
	curr_task = (TASK_DESCRIPTOR *)delay_task->exchange;
/* Subtle: the first time curr_task->link actually means task_exch->task_head
because of the layout of the exchange and task structure!! (3rd pointer)*/
//	if (!curr_task) {
//  	printinfo("rqdqtsk: delay_task %p not found\n",delay_task);
//  	rqfreeze();
//		}
	while (curr_task->link != delay_task) {
		curr_task = curr_task->link;
	     }
	if ( (curr_task->link = delay_task->link) == 0 ) {
		//task_exch->task_tail = (TASK_DESCRIPTOR *)delay_task->exchange;
		task_exch->task_tail = curr_task;
		}
	//pthread_mutex_unlock( &task_exch->mutex);
	}

#ifdef WITH_READY_LIST
/*--------------------------------------------
 enterlist:
 When a task becomes ready for running, insert it into the
 ready list according its priority, unless a suspend request
 is pending. In that case insert it into the suspend list
 instead.
 SYSTEM INTERNAL USAGE ONLY
 -------------------------------------------*/

static void enterlist (TASK_LIST_PTR task){
	
	TASK_LIST_PTR prtask,nxtask;
	
	if ( !(task->status & suspended) ) {
		if (ready == NULL) {
			printk("Enterlist: queue is empty. Add Task %6.6s\n",task_name(task));
			ready = task;
			task->link = NULL;
			return;
			}
		if (task->priority > ready->priority){
			printk("Enterlist: Add Task %6.6s at head\n",task_name(task));
			task->link = ready;
			ready = task;
			}
		else {
			prtask = ready;
			nxtask = prtask->link;
			while ((nxtask != NULL) && (nxtask->priority >= task->priority)){
				prtask = nxtask;
				nxtask = nxtask->link;
				}
			task->link = prtask->link;
			prtask->link = task;
			printk("Enterlist: Add Task %6.6s after %6.6s\n",task_name(task),task_name(prtask));
			}
		}
	else {
		if (suspend_list_root.headPtr == NULL) {
			suspend_list_root.headPtr = task;
			}
		else {
			prtask = suspend_list_root.tailPtr;
			prtask->link = task;
			}
		task->link = NULL;
		suspend_list_root.tailPtr = task;
		}
	}

/*--------------------------------------------
 removelist:
 remove a task from the list he may be in. Used for
 task deletion
 SYSTEM INTERNAL USAGE ONLY
 -------------------------------------------*/
static void removelist(TASK_DESCRIPTOR *task){
	TASK_DESCRIPTOR *CurrTask;
	if ((task->status & suspended) == 0){
		if (task == ready) {
			ready = task->link;
			}
		else {
			CurrTask = ready;
			while ((CurrTask->link != task) && (CurrTask->link != NULL)) {
				CurrTask = CurrTask->link;
				}
			if (CurrTask->link == task) {
				CurrTask->link = task->link;
				}
			}
		}
	else if (suspend_list_root.headPtr != NULL) {
		if (suspend_list_root.headPtr == task){
			suspend_list_root.headPtr = task->link;
			if (task->link == NULL) {
				suspend_list_root.tailPtr = (void *)&suspend_list_root.headPtr;
				}
			}
		else {
			CurrTask = suspend_list_root.headPtr;
			while ((CurrTask->link != task)&&(CurrTask->link != NULL)) {
				CurrTask = CurrTask->link;
				}
			if (CurrTask->link == task) {
				CurrTask->link = task->link;
				if (task->link == NULL) {
					suspend_list_root.tailPtr = (void *)&suspend_list_root.headPtr;
					}
				}
			}
		}
	}
#endif
/*---------------- Creation Functions ------------*/

void stack_prefault( void * task) {
	TASK_DESCRIPTOR *td;
	td = (TASK_DESCRIPTOR*)task;
	unsigned char dummy [MAX_SAFE_STACK];

	memset(dummy,unusedflag,MAX_SAFE_STACK);
	}

/*--------------------------------------------
  reqrun:
  run a task, when the time comes
 -------------------------------------------*/

void * reqrun(void * task) {
	TASK_DESCRIPTOR *td;
	td = (TASK_DESCRIPTOR*)task;
	int treq;

	//printinfo("Task %6.6s queued\n",task_name(td));
	if (rqstarting) {
		treq = (100-td->priority)*1000; /* time im milliseconds */
		printinfo("Task %6.6s queued - TStart = %d ms\n",task_name(td),treq/1000);
		pthread_barrier_wait(&task_barrier);
		rqstarting = false;
		/* write on stack to avoid subsequent stack fault */
		stack_prefault(task);
		/* ensure start in order of priority, in 1ms steps */
		usleep(treq);
		}
	printinfo("Task %6.6s starting\n",task_name(td));
	td->thread_start();
	}

/*--------------------------------------------
 reqctsk:
 create a task from the supplied static task descriptor
 and make it ready.
 -------------------------------------------*/


void reqctsk(STATIC_TASK_DESCRIPTOR *std) {

//DECLARE MTAB(9) BYTE DATA(0FFH,0FEH,0FCH,0F8H,0F0H,0E0H,0C0H,80H,0);
	TASK_LIST_PTR td;
	pthread_t	thread;
	int s/*,set_prio*/;

	int opt, inheritsched, use_null_attrib, policy;
	pthread_attr_t attr;
	pthread_attr_t *attrp;
	char *attr_sched_str, *main_sched_str, *inheritsched_str;
	struct sched_param param;

	enter_region;
	/*
	s = pthread_getschedparam(pthread_self(),&policy,&param);
	if (s) handle_error_en(s,"pthread_getschedparam");
#ifdef VERBOSE	
	printinfo("*Root* - ");
	display_sched_attr(policy,&param);
#endif
	if (policy == SCHED_FIFO || policy == SCHED_RR) set_prio = true;
	else set_prio = false;
	*/
	param.sched_priority = linux_priority(std->priority);
	if (set_prio) {
		s = pthread_attr_init(&attr);
		if (s != 0) {
			attrp = NULL;
			handle_error_en(s, "pthread_attr_init");
			}
		else {
			attrp = &attr;
			inheritsched = PTHREAD_EXPLICIT_SCHED;
			s = pthread_attr_setinheritsched(&attr, inheritsched);
			if (s != 0)
				handle_error_en(s, "pthread_attr_setinheritsched");
			policy = SCHED_FIFO;
			s = pthread_attr_setschedpolicy(&attr, policy);
			if (s != 0)
				handle_error_en(s, "pthread_attr_setschedpolicy");
			s = pthread_attr_setschedparam(&attr, &param);
			if (s != 0)
				handle_error_en(s, "pthread_attr_setschedparam");
			}
		}
	else attrp = NULL;

	td = std->task;
	td->nameptr = std;
	td->status = 0; // not yet started
	td->priority = param.sched_priority; //std->priority;
	td->exchange = std->exchange;
	td->thread_start = std->pc;
	s = sem_init(&td->sema,0,0);
	if (s < 0) handle_error("create task: sem_init");
	s = pthread_create(&thread,attrp,(void *)reqrun,td);
	if (s) {
		s = pthread_create(&thread,NULL,(void *)reqrun,td);
		if (s) handle_error_en(s,"create task: pthread_create");
		}
#ifdef __USE_GNU // pthread_setname is GNU only 
	pthread_setname_np(thread,std->name);
#endif
//#ifdef VERBOSE	
	s = pthread_getschedparam(thread,&policy,&param);
	if (s) handle_error_en(s,"pthread_getschedparam");
	printinfo("%6.6s - ",std->name);
	display_sched_attr(policy,&param);
//#endif
	td->thread = thread;
	td->status = running;

#ifdef WITH_READY_LIST
	enterlist(td);
#endif
	if (rq_task_head == NULL) {
		rq_task_head = rq_task_tail = td;
		}
	else {
		rq_task_tail->tasklink = td;
		rq_task_tail = td;
		}
	td->tasklink = NULL;


	exit_region(active_task());
	}

/*--------------------------------------------
 reqcxch:
 Create (i.e. initialize) the exchange supplied
 -------------------------------------------*/

//void reqcxch(EXCHANGE_LIST_PTR exch) {
void reqcxch(EXCHANGE_LIST_PTR exch,const char *name) {

	int namelen;
	printk("Creating exchange %p %6.6s\n",exch,name);	
	pthread_mutex_lock(&mutex1); /* avoid conflicts */
	exch->message_head = NULL;
	exch->task_head = NULL;
	exch->message_tail = (void *)exch;
	exch->task_tail = (void *)exch;
	memset(exch->name,0,sizeof(exch->name));
	namelen = strlen(name);
	if (namelen > EXCH_NAME_LEN) namelen = EXCH_NAME_LEN;
	memcpy(exch->name,name,namelen);

	if (rq_exchange_head == NULL) {
		rq_exchange_head = rq_exchange_tail = exch;
		}
	else {
		rq_exchange_tail->exchange_link = exch;
		rq_exchange_tail = exch;
		}
	exch->exchange_link = 0;
	bzero(&exch->mutex,sizeof(exch->mutex));
	pthread_mutex_unlock(&mutex1);
	printk("Exchange %6.6s created\n",exch->name);	
	}

/*--------------------------------------------
 reqcmsg:
 Create (i.e. initialize) the message supplied
 -------------------------------------------*/
void reqcmsg(MSG_DESCRIPTOR * msg, int msgsize) {

	msg->length = msgsize;
	msg->link = (void *)active_task();
	msg->type = 0x40; // just to avoid system defined types
	msg->yltime = rqsystime;
	msg->home_exchange = msg->response_exchange = NULL;
	}

/*---------------- Deletion Functions ------------*/

/*--------------------------------------------
 reqdtsk:
 delete a task, by removing it from the system lists
 -------------------------------------------*/
void reqdtsk (TASK_DESCRIPTOR *task){
	TASK_DESCRIPTOR *ctask;
	EXCHANGE_DESCRIPTOR *exch;
	TASK_LIST_PTR rqactv;

	rqactv = active_task();
	printk("reqdtsk executing Task %6.6s rqactv= %6.6s\n",task_name(task),task_name(rqactv));
	if((task->status &(waiting | delayed))) {
		if (task->exchange != NULL) {
			reqdqtsk(task);
			if (task->status & delayed){ cancel_delay (task);}
			}
		/*printk("pthread_cancel %6.6s\n",task_name(task));*/
		}

#ifdef DEL_FROM_LIST

	if (rq_task_head == task) {
		rq_task_head = task->tasklink;
		if (task->tasklink == NULL) {
			rq_task_tail = (void *)&rq_task_head;
			}
	} else {
		ctask = rq_task_head;
		while ((ctask != NULL) &&(ctask->tasklink != task)) {
			ctask = ctask->tasklink;
			}
		if (ctask->tasklink == task) {
			ctask->tasklink = task->tasklink;
			if (task->tasklink == NULL) {
				rq_task_tail = (void *)&rq_task_head;
				}
			}
		}
#endif

	task->status = deleted;
	if (task == rqactv) {
		pthread_exit(NULL);
		}
	else pthread_cancel(task->thread);
	}

/*--------------------------------------------
 reqdxch:
 exchange deletion check
 returns true if the exchange in unused, and can be safely removed
 false if messages or tasks are queued on it
 -------------------------------------------*/
int reqdxch (EXCHANGE_LIST_PTR exch) {
	int result;
	volatile EXCHANGE_LIST_PTR prev;
	volatile EXCHANGE_LIST_PTR curr;

	enter_region;
	result = false;
	if ((exch->message_head == NULL) && (exch->task_head == NULL)){
		result = true;
		/*printk("reqdxch: deleting %6.6s_EX\n",exch->name);*/
		}
	//else /*printk("reqdxch: can't delete %6.6s_EX\n",exch->name);*/
	if (result) {
		result = false;
		prev = rq_exchange_head;
		while (prev && prev->exchange_link != exch)
			prev = prev->exchange_link;
		if (prev) {
			/*printk("reqdxch: %6.6s_EX found\n",exch->name);*/
			curr = prev->exchange_link;
			prev->exchange_link = curr->exchange_link;
			if (rq_exchange_tail == curr) rq_exchange_tail = prev;
			result = true;
			}
		}
	exit_region(active_task());
	return result;
	}

/*-------------  Suspend / Resume Task --------------------*/

/*
 * Rather superfluous, because they're asynchronous, but may
 * be handy during debug.
 */

/*--------------------------------------------
 reqsusp:
 suspend a task, if ready. Otherwise tag it for suspension.

 Note - suspending an already suspended task is legal, but has no
 effect whatsoever. There isn't anything like a suspension counter
 or such stuff.
 -------------------------------------------*/

void reqsusp (TASK_DESCRIPTOR *task){
	TASK_DESCRIPTOR *ctask;
	int fail;
#ifdef WITH_READY_LIST
	ctask = ready; // scan ready list
	while ((ctask != task) && (ctask != NULL)) {
		ctask = ctask->link;
		}
	if (ctask == task) removelist(task);
#endif
	task->status |= suspended;
#ifdef WITH_READY_LIST
	if (ctask == task) enterlist(task);
#endif
	if (task == active_task()) {
		task->status |= needpost;
		fail = sem_wait(&task->sema);
		task->status &=~(suspended | needpost);
		}
	}

/*--------------------------------------------
 reqresm:
 resume a suspended task.

 Note - resuming a task which is not suspended is legal, but has no
 effect whatsoever. There isn't anything like a suspension counter
 or such stuff.
 -------------------------------------------*/

void reqresm (TASK_DESCRIPTOR *task){
#ifdef WITH_READY_LIST
	TASK_DESCRIPTOR *ctask;
	ctask = suspend_list_root.headPtr; // scan suspend list
	while ((ctask != task) && (ctask != NULL)) {
		ctask = ctask->link;
		}
	if (ctask == task) removelist(task);
#endif
	if (task->status & suspended ) {
		task->status &= ~suspended;
#ifdef WITH_READY_LIST
		if (ctask == task) enterlist(task);
#endif
		if (task->status & needpost) {
			task->status &= ~needpost;
			sem_post(&task->sema);
			}
		}
	}

/*-------------  Suspend / Resume Full system --------------------*/

/*--------------------------------------------
 reqfreeze:

 Freezes yalrt and give control back to Linux. Handy for debug
 or prior to termination, without worrying about dependencies.
 exchange deletion check
 -------------------------------------------*/

void reqfreeze (void) {
	TASK_LIST_PTR rqactv;
	int rc;

	if (rqfrozen) return;
	printinfo("System frozen\n");
	enter_region;
	rqactv = active_task();
	rqfrozen = true;
	if (rqactv != &RootTaskTd) {
		rc = pthread_mutex_lock(&fmutex);
		while (rqfrozen) {
			printinfo("rqfreeze: Task %6.6s frozen\n",task_name(rqactv));
			rc = pthread_cond_wait(&unfreeze,&fmutex);
			printinfo("rqfreeze: Task %6.6s unfrozen\n",task_name(rqactv));
			}
		rc = pthread_mutex_unlock(&fmutex);
		}	
	//exit(1);
	}

/*--------------------------------------------
 reqbake:

 Bakes a deep frozen yalrt and resumes operation.

 -------------------------------------------*/

void reqbake (void) {
	int rc;
	if (rqfrozen) {
		printinfo("System restarting\n");
		rqfrozen = false;
		rc = pthread_cond_broadcast(&unfreeze);
		}
	}
/*------------------ THE REAL STUFF - THE SCHEDULER -------*/


/* There's no more a scheduler: we rely on Linux scheduling!!*/

/*--------------------------------------------
 exit_region:
 whenever we get out of the protected region, the currently active task
 may be no more at the top of the ready list, so we must reschedule
 SYSTEM INTERNAL USAGE ONLY
 -------------------------------------------*/

void exit_region(TASK_DESCRIPTOR *task) {
	int fail;
#ifdef BY_STEP
	if (!rqstarting) rqfreeze();
#endif
	if (task->status & suspended) {
		if (task->status & needpost == 0) {
			task->status |= needpost;
			fail = sem_wait(&task->sema);
			task->status &=~(suspended | needpost);
			}
		}
	if (rqfrozen) {
		fail = pthread_mutex_lock(&fmutex);
		while (rqfrozen) {
			printinfo("exit_region: Task %6.6s frozen\n",task_name(task));
			fail = pthread_cond_wait(&unfreeze,&fmutex);
			printinfo("exit_region: Task %6.6s unfrozen\n",task_name(task));
			}	
		fail = pthread_mutex_unlock(&fmutex);
		}
#ifndef EARLY_RUN
	task->status = running;
#endif
	return;
	}
/*-----------Message passing SEND, WAIT, ACCEPT -----------*/

/*--------------------------------------------
 reqsend:
 Send a message to an exchange. If a task was waiting at that
 exchange, then it's made ready.
 -------------------------------------------*/
void reqsend (EXCHANGE_LIST_PTR exch, MSG_DESCRIPTOR *msg) {

	MSG_DESCRIPTOR *previous_message;
	TASK_LIST_PTR task;
	TASK_LIST_PTR rqactv;

	enter_region;
	rqactv = active_task();
	pthread_mutex_lock( &exch->mutex);
	printk("Task %6.6s entering RqSend - Exchange %6.6s_EX - msg %p\n",task_name(rqactv),exch->name,msg);
	if ((msg->link != (void *)rqactv) && (msg->link != (void *)msg)){
		user_error(MSG_ERR, (void*)msg);
		rqfreeze();
		exit_region(rqactv);
		return; // useless but...
		}
	if (exch->task_head == NULL ){
		/*printk("No task waiting\n");*/
		if (exch->task_tail != (void *) exch) {
			user_error(EXCH_ERR,(void *)exch);
			rqfreeze();
			exit_region(rqactv);
			return; // useless but...
			}
		previous_message = exch->message_tail;
		previous_message->link = msg;
		exch ->message_tail = msg;
		msg->link = NULL;
		pthread_mutex_unlock( &exch->mutex);
		}
	else {
		if ((exch->message_head != NULL ) || (exch->message_tail != (void *) exch)) {
			user_error(EXCH_ERR,(void *)exch);
			rqfreeze();
			exit_region(rqactv);
			return; // useless but...
			}
		task = exch->task_head;
		/*printk("Task %6.6s waiting\n",task_name(task));*/
		if ((exch->task_head = task->link) == NULL) {
			exch->task_tail = (void *) exch;
			}
		if (msg->type > timeout_type) msg->link = (void *)task;
		else msg->link = (void *)&msg->link;
		task->message = msg;
		task->status &= ~waiting;
		if (task->status && delayed) {  // delayed
			cancel_delay(task);
			}
#ifdef WITH_READY_LIST
		enterlist(task);
#endif
		// unlock the exchange before a possible context switch
		pthread_mutex_unlock( &exch->mutex);
		sem_post(&task->sema);
		}
	printk("Task %6.6s exiting RqSend\n",task_name(rqactv));
	}
/*--------------------------------------------
 reqacpt:
 fetch message from an exchange and make it
 available to currently active task - if no message
 is available return NULL
 -------------------------------------------*/
MSG_DESCRIPTOR * reqacpt (EXCHANGE_LIST_PTR exch) {
	TASK_LIST_PTR rqactv;

	enter_region;
	rqactv = active_task();
	pthread_mutex_lock( &exch->mutex);
	rqactv->exchange = exch;
	rqactv->message = NULL;
	if (exch->message_head != NULL){ reqdqmsg((INT_EXCHANGE_LIST_PTR)exch,rqactv); }
	pthread_mutex_unlock( &exch->mutex);
	exit_region(rqactv);
	return rqactv->message;
	}
/*--------------------------------------------
 reqwait:
 Wait for a message at an exchange. If no message is available
 the requesting task is put to wait either forever, or for the requested
 amount of time. If the time expires without a message being available
 the delay task will handle that.
 -------------------------------------------*/

MSG_DESCRIPTOR *reqwait(EXCHANGE_LIST_PTR exch, unsigned short delay){

	static TASK_LIST_PTR tail_task;
	static TASK_LIST_PTR delay_task;

	unsigned short t_delay;

	struct timespec CurrTime;
	unsigned int adelay;
	TASK_LIST_PTR rqactv;
	int fail;
	int sem_units;


	enter_region;
	//printk("Thread %p entering Rqwait - Exchange %6.6s_EX - delay=%d\n",pthread_self(),exch->name,delay);
	rqactv = active_task();
	//printk("rqactv = %p\n",rqactv);
	if (rqactv == NULL) {
		printinfo("Reqwait: thread %p  task = NULL pointer\n",pthread_self());
		rqfreeze();
		}
	printk("Task %6.6s entering Rqwait - Exchange %6.6s_EX - delay=%d\n",task_name(rqactv),exch->name,delay);
	pthread_mutex_lock( &exch->mutex);
	rqactv->exchange = exch;
	//rqactv->message = NULL; // to tell apart real and false timeouts
	if (exch->message_head == NULL ) {
#ifdef WITH_READY_LIST
		ready = rqactv->link;
#endif
		rqactv->link = NULL; /* This is going to be the last one */
		if (exch-> task_head == NULL) {
		  exch->task_head = rqactv;
		  exch->task_tail = rqactv;
		  }
		else {
		  tail_task = exch->task_tail;
		  tail_task->link = rqactv;
		  exch->task_tail = rqactv;
		  }
		if (rqactv->link == rqactv) {
			printinfo("Ooops: rqwait Task %6.6s at Exch %6.6s_EX points to itself!\n",task_name(rqactv),exch->name);
			rqfreeze();
			}
		if (delay) {
			rqactv->status = delayed;
			rqactv->delay = delay;
			pthread_mutex_lock(&delay_list_mutex);
			delay_task = (void*)&rq_delay_list_head;
			while (rqactv->delay > (t_delay = delay_task->delay)) {
				rqactv->delay -= t_delay;
				delay_task = delay_task->next;
				}
			delay_task->delay = t_delay - rqactv->delay;
			rqactv->prev = delay_task->prev;
			delay_task->prev = rqactv;
			rqactv->next = delay_task;
			delay_task = rqactv->prev;
			delay_task->next = rqactv;
			rq_delay_list_tail.delay = 0xFFFF;
			pthread_mutex_unlock(&delay_list_mutex);
			}
		else {
			rqactv->status = waiting;
			}
		pthread_mutex_unlock( &exch->mutex);
		fail = sem_wait(&rqactv->sema);
		pthread_mutex_lock( &exch->mutex);
		/* Here the message has been received */
#ifdef EARLY_RUN
		rqactv->status = running;
#else
		rqactv->status = 0;
#endif
		if (rqactv->link == rqactv) {
			printinfo("Task %6.6s at exch %6.6s_EX pointing to itself!\n",task_name(rqactv),exch->name);
			pthread_mutex_unlock( &exch->mutex);
			rqfreeze();
			}
		else pthread_mutex_unlock( &exch->mutex);
		}
	else {
		//printk("rqwait:  reqdqmsg\n");
		reqdqmsg((INT_EXCHANGE_LIST_PTR)exch,rqactv);
		pthread_mutex_unlock( &exch->mutex);
		}
	// hard_sti();
	printk("Task %6.6s exiting Rqwait\n",task_name(rqactv));
	/*printk("Task %6.6s return from Rqwait: Msg= %p\n",task_name(rqactv),rqactv->message);*/
	if (rqactv->message == NULL) {
		user_error(MSG_NULL_ERR,exch);
		rqfreeze();
		}
	exit_region(rqactv);
	return rqactv->message;
	}

/*----------------timer related functions--------------*/

TASK_DESCRIPTOR rqtimerTd;

STATIC_TASK_DESCRIPTOR rqtimerStd =
	STD_INITIALIZER("*Timer",rqtimer,1,rqtimerTd);

/*--------------------------------------------
 rqtimer
 Emulates the timer interupt handler.
 It wakes up at every tick and then performs YRMX service.
 It checks whether we have reached the proper count for
 the next task in the delay list.
 If so it just queues the interrupt message in the isnd queue,
 and leaves the delay task to take over.
 Optionally it increments system timer. 
---------------------------------------------*/

static void rqtimer (void) {
	struct timespec tnext;
	int err,isnd;
	TASK_DESCRIPTOR *waiting_task;

	clock_gettime(CLOCK_MONOTONIC,&TimeOrigin);
	tnext = tsAdd(TimeOrigin,rqtick);
	timetest = reqsystime();
	forever {
		err = clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&tnext,NULL);
		if (!err) {
#ifdef PROFILE_TIMER
		int diff;
		struct timespec tsdiff;
		int tdiff;
		if (clock_gettime(CLOCK_MONOTONIC, &tnow) == -1)
			handle_error("clock_gettime");
		diff = tsCompare(tnow,tnext); // 1 tnow > tnext; -1 tnow < tnext; 0 tnow = tnext 
		if (diff < 0) {
			tsdiff = tsSubtract(tnext,tnow);
			tdiff = -tsdiff.tv_nsec;
			}
		else if (diff > 0) {
			tsdiff = tsSubtract(tnow,tnext);
			tdiff = tsdiff.tv_nsec;
			}
		else {
			tdiff = 0;
			}
		// compute min/max and accumulate for average;
		if (tdiff < tmin) tmin = tdiff;
		else if (tdiff > tmax) tmax = tdiff;
		tsum += tdiff;
		// Compute Variance
		if (ntick == 0) {
			K = tdiff;
			Ex = 0;
			Ex2 = 0;
			//timetest = 0; // To verify ntick
			}
		ntick++;
		Ex += (tdiff - K);
		Ex2 += (tdiff - K) * (tdiff - K);
#endif
			timetest++;
			isnd=false;
			pthread_mutex_lock(&delay_list_mutex);
			waiting_task = rq_delay_list_head.next;
			if (waiting_task->delay != 0xFFFF) {
				if (!waiting_task->delay){
					printk("rqtimer 1 - task %6.6s ready\n",task_name(waiting_task));
					isnd = true;
					}
				else if (!--waiting_task->delay){
					printk("rqtimer 2 - task %6.6s ready\n",task_name(waiting_task));
					isnd = true;
					}
				}
			pthread_mutex_unlock(&delay_list_mutex);
			if (isnd) reqisnd(&rq_delay_exchange); 
			tnext = tsAdd(tnext,rqtick);
			}
		}
	}
/*--------------------------------------------
 rqdelaytask:
 It handles the "slow" timer interrupt service, when needed:
 it removes the appropriate task(s) from the delay list,
 it removes them from the exchange queue where they were waiting and
 puts them on the ready list.
 -------------------------------------------*/
TASK_DESCRIPTOR rqdelaytaskTd;

STATIC_TASK_DESCRIPTOR rqdelaytaskStd =
	STD_INITIALIZER("*DELAY",rqdelaytask,2,rqdelaytaskTd);

static void rqdelaytask (void) {

	TASK_LIST_PTR delay_task;
	TASK_LIST_PTR curr_task;
	EXCHANGE_DESCRIPTOR *task_exch;
	MSG_DESCRIPTOR *t_msg;

	 forever {
		t_msg = reqwait((EXCHANGE_DESCRIPTOR *)&rq_delay_exchange, 0);
		printk("rqdelaytask called\n")
		enter_region;
		pthread_mutex_lock(&delay_list_mutex);
		delay_task = rq_delay_list_head.next;
		while (delay_task->delay == 0) {
			task_exch = delay_task->exchange;
			curr_task = (TASK_DESCRIPTOR *)delay_task->exchange;
/* Subtle: the first time curr_task->link actually means task_exch->next
because of the layout of the exchange and task structure!! */
			while (curr_task->link != delay_task) {
				curr_task = curr_task->link;
				}
			if ( (curr_task->link = delay_task->link) == 0 ) {
				task_exch->task_tail = curr_task;
				}
			cancel_delay_unsafe (delay_task); /* questo fa rqdlvl se serve */
//			printk("Switching to TASK %p",&delay_task);
			delay_task->message = (MSG_DESCRIPTOR *)&time_out_message;
//			delay_task->delay = 0xFFFF;
			/*printinfo("rqdelaytask sending message %p to task %6.6s \n",
			 delay_task->message, task_name(delay_task));*/
			sem_post(&delay_task->sema);
			delay_task = rq_delay_list_head.next;
			}
		pthread_mutex_unlock(&delay_list_mutex);
		exit_region(active_task());
		}
	}

/*----------------interrupt related functions--------------*/

/*--------------------------------------------
 reqisnd:
 It was used by an interrupt handler to append a message to the
 appropriate interrupt exchange, so that interrupt related tasks
 can perform their action. It is also used by the default handler
 Now it just performs an reqsend.
 -------------------------------------------*/
void reqisnd (INT_EXCHANGE_DESCRIPTOR *int_ex) {

	printk("reqisnd - enter\n");
	if ((void *)int_ex->link == (void *)&int_ex->link){
		printk("reqisnd - rqsend\n");
		int_ex->type = int_type;
		reqsend((EXCHANGE_DESCRIPTOR *)int_ex,(void *)&int_ex->link);
		}
	else { printk("Missed Int\n");int_ex-> type = missed_int_type; }
	}

/*-----------SETUP and CLEANUP stuff -----------*/

int reqstart (STD_LIST_PTR itt[],int ntask,EXCHANGE_LIST_PTR iet[],/*EXCH_NAME ien[],*/int nexch) {
//int reqstart (STD_LIST_PTR (*itt),EXCHANGE_LIST_PTR (*iet)) {
	int i,j,s;
	char name [EXCH_NAME_LEN+1];
	STD_LIST_PTR a_task;
	EXCHANGE_LIST_PTR an_exch;
	struct sched_param param;
	int policy;
	int count;
#ifdef USE_AFFINITY
	cpu_set_t	mask;
#endif

	// let's see if selected policy support thread priorities
	s = pthread_getschedparam(pthread_self(),&policy,&param);
	if (s) handle_error_en(s,"pthread_getschedparam");
//#ifdef VERBOSE	
	printinfo("*Root* - ");
	display_sched_attr(policy,&param);
//#endif
	if (policy == SCHED_FIFO || policy == SCHED_RR) set_prio = true;
	else set_prio = false;
#ifdef USE_AFFINITY
	CPU_ZERO(&mask);
	CPU_SET(0, &mask);
	CPU_SET(1, &mask);
	s = sched_setaffinity(0, sizeof(mask), &mask);
	if (s) handle_error_en(s,"sched_settaffinity");
#endif
	// Lock memory
	s = mlockall(MCL_CURRENT | MCL_FUTURE);
	if (s) handle_error_en(s,"mlockall failed");

	// We must set up a barrier to avoid starting tasks before
	// all have been created
	count = 2; // system tasks
	for (i=0; i<ntask; i++) {
		a_task = itt[i];
		if (a_task != NULL) count++;
		}
	pthread_barrier_init(&task_barrier,NULL,count);

	/*printk("reqstart starting\n");*/
	rq_task_head = rq_task_tail = &RootTaskTd;

#ifndef WITH_READY_LIST
	ready = &RootTaskTd;
#endif

	RootTaskTd.link = NULL;
	RootTaskTd.priority = linux_priority(RootTaskStd.priority);
	RootTaskTd.nameptr = &RootTaskStd;
	RootTaskTd.status = running;
	RootTaskTd.master_mask = RootTaskTd.slave_mask = 0;
	RootTaskTd.thread = pthread_self();

	rq_delay_list_head.next = rq_delay_list_head.prev = (void*)&rq_delay_list_tail;
	rq_delay_list_head.link = NULL;
	rq_delay_list_head.delay = 0;

	rq_delay_list_tail.next = rq_delay_list_tail.prev = (void*)&rq_delay_list_head;
	rq_delay_list_tail.link = NULL;
	rq_delay_list_tail.delay = 0xFFFF;

	time_out_message.link = (MSG_DESCRIPTOR *)&time_out_message;
	time_out_message.length = sizeof(time_out_message);
	time_out_message.type = timeout_type;

	time_out_message.link = NULL; // system message, can't be sent

	if (clock_gettime(SYSTEM_CLOCK, &StartTime) == -1)
		handle_error("clock_gettime");
	// system exchanges
	j = 0;
	an_exch = (EXCHANGE_LIST_PTR)&rq_delay_exchange;
	initial_exchange_table[j++] = an_exch;
	memset(name,0,EXCH_NAME_LEN+1);
	memcpy(name,an_exch->name,EXCH_NAME_LEN);
	reqcxch(an_exch,name);
	rq_delay_exchange.link = (void *)&rq_delay_exchange.link;
	//reqcxch((EXCHANGE_LIST_PTR)&rq_delay_exchange,&rq_delay_exchange.name);
	// user exchanges
	for (i=0; i<nexch; i++) {
		initial_exchange_table[j++] = an_exch = iet[i];
		if (an_exch != NULL) {
		/* we can't copy exchange name over itself ! */
			memset(name,0,EXCH_NAME_LEN+1);
			memcpy(name,an_exch->name,EXCH_NAME_LEN);
			reqcxch(an_exch,name);
			}
		}
	// system tasks
	printk("Creating Task: %6.6s \n",rqtimerStd.name);
	rqctsk(&rqtimerStd);
#ifdef WITH_READY_LIST
	ready = NULL; 
#endif
	printk("Creating Task: %6.6s \n",rqdelaytaskStd.name);
	rqctsk(&rqdelaytaskStd);
	// user tasks
	for (i=0; i<ntask; i++) {
		initial_task_table[i] = a_task = itt[i];
		if (a_task != NULL) {
			printk("Creating Task: %6.6s \n",a_task->name);
			reqctsk(a_task);
			}
		}
#ifdef VERBOSE
	display_task_list();
	display_exchange_list();
#endif // VERBOSE
#ifdef WITH_READY_LIST
	display_ready_list();
#endif
	s = 0;
	return s;
	}

void display_ready_list(void) {
	
	TASK_LIST_PTR this_task;
	STATIC_TASK_DESCRIPTOR *this_std;

	printinfo("\n-------- Ready List --------\n" );

	//this_task = ready; //rq_task_head;
#ifdef WITH_READY_LIST
	this_task = ready;
#else
	this_task = rq_task_head;
#endif
	while (this_task != NULL) {
		this_std = this_task->nameptr;
		if (this_std == NULL) {
			printinfo("Ooops, Task list Std, null Pointer\n");
			return;
			}
		if (this_task->status & running) {
			printinfo("%6.6s ",this_std->name);
			printinfo("st = %2x ",this_task->status);
			//printinfo("last excg = %6.6s_EX ",this_task->exchange->name);
			//printinfo("last msg = %p ",this_task->message);
			printinfo("pri = %d \n",this_task->priority);
			if (this_task->link == this_task) {
				printinfo(" Ooops! task %6.6s pointing to itself\n",this_std->name);
				//return;
				}
			}
#ifdef WITH_READY_LIST
		this_task = this_task->link;
#else
		this_task = this_task->tasklink;
#endif
		}
	printinfo("\n");
	}
	
void display_task_list(void) {
	
	TASK_LIST_PTR this_task;
	STATIC_TASK_DESCRIPTOR *this_std;

	printinfo("\n-------- Task List --------\n" );
	printinfo(" Name  st     exch    last msg  pri \n");
	this_task = rq_task_head;
	while (this_task != NULL) {
		this_std = this_task->nameptr;
		if (this_std == NULL) {
			printinfo("Ooops, Task list Std, null Pointer\n");
			return;
			}
		printinfo("%6.6s ",this_std->name);
//		printinfo("thread =%p ",this_task->thread);
//		printinfo("task =%p ",this_task);
		printinfo("%2.2X ",this_task->status);
		if (this_task->exchange) 
			printinfo(" %6.6s_EX ",this_task->exchange->name);
		else printinfo(" %9.9s ","(nil)");
		if (!this_task->message) printinfo("    ");
		printinfo(" %p ",this_task->message);
		printinfo(" %2.2d \n",this_task->priority);
		if (this_task->link == this_task) {
			printinfo(" Ooops! task %6.6s pointing to itself\n",this_std->name);
			//return;
			}
		this_task = this_task->tasklink;
		}
	printinfo("\n---------------------------\n");
	}
	
void display_exchange_list(void) {
	int t=0;
	EXCHANGE_LIST_PTR this_exch;
	MSG_DESCRIPTOR *this_msg;
	TASK_LIST_PTR this_task;
	STATIC_TASK_DESCRIPTOR *this_std;
	char name [EXCH_NAME_LEN+1];

	printinfo("\n-------- Exchanges --------\n" );

	this_exch = rq_exchange_head;
	while (this_exch != NULL) {
		this_msg = this_exch->message_head;
		this_task = this_exch->task_head;
		
		if ((this_msg != NULL)||(this_task != NULL) /*|| true*/) {
			memset(name,0,EXCH_NAME_LEN+1);
			memcpy(name,this_exch->name,EXCH_NAME_LEN);
			printinfo("\nExchg: %s_EX ",name);
			}
		if (this_msg != NULL) {
			t = 0;
			printinfo("Queued Messages: ");
			}
		while (this_msg != NULL) {
			if (++t > 3) {
				printinfo("...");
				break;
				}
			else printinfo(" %p type= %2X",this_msg,this_msg->type);
			this_msg = this_msg->link;
			}
		
		if (this_task != NULL) {
			t = 0;
			printinfo("Queued Tasks: ");
			}
		while (this_task != NULL) {
			this_std = this_task->nameptr;
			if (this_std == NULL) {
				printinfo("Ooops, Task list Std, null Pointer\n");
				return ;
				}
			if (++t > 3) {
				printinfo("...");
				break;
				}
			else printinfo("%6.6s ",this_std->name);
			this_task = this_task->link;
			}
		this_exch = this_exch->exchange_link;
		}
	
	printinfo("\n");
	return;
	}

void display_delay_list(void) {
	TASK_LIST_PTR this_task;
	STATIC_TASK_DESCRIPTOR *this_std;
	int t;

	printinfo("\n-------- Delay List --------\n" );
	printinfo(" Name   Delay    exch    last msg   pri \n");

	t=0;
	this_task = rq_delay_list_head.next;
	if (this_task == NULL) {
		printinfo("Ooops,  Delay list Td null Pointer\n");
		return;
		}
	while (this_task->delay != 0xFFFF) {
		this_std = this_task->nameptr;
		if (this_std == NULL) {
			printinfo("Ooops, delay list Std, null Pointer\n");
			return;
			}
		t += this_task->delay;
		printinfo("%6.6s ",this_std->name);
		printinfo("%5.0d ",t);
		if (this_task->exchange) 
			printinfo(" %6.6s_EX ",this_task->exchange->name);
		else printinfo(" %9.9s ","(nil)");
		if (!this_task->message) printinfo("    ");
		printinfo(" %p ",this_task->message);
		printinfo(" %2.2d \n",this_task->priority);
		if (this_task->link == this_task) {
			printinfo(" Ooops! task %6.6s pointing to itself\n",this_std->name);
			//return;
			}
		this_task = this_task->next;
		}
#ifdef PROFILE_TIMER
	Mean = (K + Ex) / ntick;
	Var = sqrt((Ex2 - (Ex*Ex)/ntick) / (ntick-1));
	printinfo("timer error: samples %d min=%d ns max=%d ns avg=%f ns\n Variance=%f\n",ntick,tmin,tmax,Mean,Var);
#endif
	printinfo("rqsystime = %d timetest= %d\n",reqsystime(),timetest);
	printinfo("\n---------------------------\n");
	}

