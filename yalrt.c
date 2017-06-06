/*
COPYRIGHT (C) 2001-2005  Giuliano Colla - Copeca srl (colla@copeca.it)

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

/*
ACKNOWLEDGMENTS:
YALRT (Yet Another Linux Real Time) is the result of combining a clone
of the Intel's RMX80 nucleus with a hardware abstraction layer.

- Robert Kahn in the mid 70's published in the IEEE proceedings the basic
  concepts for Intel's RMX80 nucleus. Due to the limited power of CPU's at that
  time he was very careful to provide the utmost efficiency, by smart data
  structures. All the bright ideas come from him, while I take   full
  responsibility for the sloppy implementation of the resulting scheduler.

- Wherever appropriate pieces of code from RTAI code have been lifted and
  shamelessly included into Yalrt.

*/

/* yalrt.c - the nucleus of YALRT */

#define MSG_ERR 1
#define EXCH_ERR 2
#define INT_ERR 3

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/version.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/console.h>
#include <asm/param.h>
#include <asm/system.h>
#include <asm/hw_irq.h>
#include <asm/irq.h>
#include <asm/desc.h>
#include <asm/io.h>
#include <asm/mmu_context.h>
#include <asm/uaccess.h>
#include <asm/unistd.h>
#ifdef CONFIG_X86_LOCAL_APIC
#include <asm/fixmap.h>
#include <asm/bitops.h>
#include <asm/mpspec.h>
#ifdef CONFIG_X86_IO_APIC
#include <asm/io_apic.h>
#endif /* CONFIG_X86_IO_APIC */
#include <asm/apic.h>
#endif /* CONFIG_X86_LOCAL_APIC */
//#define __RTAI_HAL__
//#include <asm/rtai_hal.h>
//#include <asm/rtai_lxrt.h>
#ifdef CONFIG_PROC_FS
#include <linux/stat.h>
#include <linux/proc_fs.h>
//#include <rtai_proc_fs.h>
#endif /* CONFIG_PROC_FS */
#include <stdarg.h>
//#include <rtai_hal_names.h>
//#include <asm/rtai_oldnames.h>
//#include <rtai_config.h>
#include <linux/ipipe.h>
#include <yalrt.h>

#define save_all_reg() \
	__asm__ __volatile__( \
	"pushl %eax\n\t" \
	"pushl %ebp\n\t" \
	"pushl %edi\n\t" \
	"pushl %esi\n\t" \
	"pushl %edx\n\t" \
	"pushl %ecx\n\t" \
	"pushl %ebx\n\t" )

#define restore_all_reg() \
	__asm__ __volatile__( \
	"popl %ebx\n\t" \
	"popl %ecx\n\t" \
	"popl %edx\n\t" \
	"popl %esi\n\t" \
	"popl %edi\n\t" \
	"popl %ebp\n\t" \
	"popl %eax\n\t")

#define NANO 1000000000LL

MODULE_DESCRIPTION("YALRT nanokernel");
MODULE_AUTHOR("Giuliano Colla <colla@copeca.it>");
MODULE_LICENSE("GPL");

#define YALRT_DOMAIN_ID  0x59414C52
#define YALRT_VERSION "v2.0"

static struct ipipe_domain yalrt_domain;

static unsigned yalrt_sysreq_virq = 0;

CONTEXT_SWITCH_DATA linux_context_data[2];

/*
static unsigned long yalrt_sysreq_map = 1; // srq 0 is reserved

static spinlock_t yalrt_lsrq_lock = SPIN_LOCK_UNLOCKED;

static int yalrt_event_handler (unsigned event, void *evdata) {
	return 0;
	}
*/
void exit_region(int FromInt);
void switch_to_linux(void);
void switch_to_yalrt(void);
void switch_domain(unsigned virq);

void user_error(int, void *);
void reqelvl(int);
void reqdlvl(int);
void reqsetv(int irq,void (*handler)(void));
void rqrun(void);
static void rqdelaytask(void);

#ifdef yalrtdbg
struct tasklist {
		TASK_LIST_PTR task;
		unsigned int time;
		};
#define Task_log_size	4096
struct tasklist task_log[Task_log_size];
int History;
struct int_list {
	TASK_LIST_PTR task;
	unsigned int time;
	unsigned int level;
	};
#define Int_log_size	256
struct int_list int_log[Int_log_size];
int History_i;
#endif

TASK_LIST_PTR ready = NULL;

TASK_LIST_PTR saved_ready = NULL;

TASK_LIST_PTR rqactv = NULL;

TASK_LIST_PTR saved_rqactv = NULL;

int rqfrozen = 0;

struct task_queue_root {
	TASK_LIST_PTR headPtr;
	TASK_LIST_PTR tailPtr;
	};

struct task_queue_root suspend_list_root ={NULL,(void *)&suspend_list_root};

struct delay_list_queue {
	task_links;
	};

struct delay_list_queue rq_delay_list_head,rq_delay_list_tail;

//unsigned char queue_index;
int queue_index = 0;

INT_EXCHANGE_LIST_PTR isnd_queue[256];

TASK_LIST_PTR rq_last_ndp_task = NULL;

unsigned int rq_sys_excep_ptr;

int critical_region_flag = 0;


unsigned int rqsystime = 0;
unsigned long StartTime;
unsigned long systime[32];
unsigned long long linux_time,linux_next = 0;
unsigned hz,latch; /* to understand what adeos does with timer */
unsigned int Ratio = 9;

struct sys_msg time_out_message = {
		NULL,
		sizeof(time_out_message),
		timeout_type
		};

unsigned char interrupt_mask[8];

unsigned char disable_mask[8];

int max_queue = 0;

EXCHANGE_LIST_PTR rq_exchange_head = NULL;
EXCHANGE_LIST_PTR rq_exchange_tail = (void *)&rq_exchange_head;

TASK_LIST_PTR rq_task_head = NULL;
TASK_LIST_PTR rq_task_tail = (void *)&rq_task_head;

INT_EXCHANGE_DESCRIPTOR rqlintex[NR_GLOBAL_IRQ] ;

TASK_DESCRIPTOR IdleTask,DelayTd;

STATIC_TASK_DESCRIPTOR IdleStd ={
	"*Idle*",
	NULL,
	NULL,
	0,
	0,
	0xFF,
	NULL,
	&IdleTask,
	0};

STATIC_TASK_DESCRIPTOR DelayStd ={
	"*Delay",
	NULL,
	NULL,
	0,
	0,
	0xFF,
	NULL,
	&DelayTd,
	0};

/* Standard Yalrt timer is 1 ms = 1000 Hz while standard x8s Linux
   timer used to be 10 ms = 100 Hz. Now also Linux is 1000 Hz so
   we most likely may live it alone.
   We don't care about nanoseconds, but we must cope with adeos
   which does */

#define TICK 1000000 //ns (!!!!! CAREFULL NEVER GREATER THAN 10000000 !!!!!)

int rqsystick = TICK;

static void yalrt_timer_handler(unsigned int irq, void *cookie);

void rqrun(void){
// currently tasks run in kernel space so we don't need
// to set the ds
	return;
	}
/*---------------- utility procedures --------------*/

static void printdomain(void) {
	if (ipipe_current_domain == &yalrt_domain)
		printk("Current domain = yalrt\n");
	
	else printk("Current domain = root\n");
	}

static inline int rt_save_switch_to_real_time(int cpuid)
{
	SET_TASKPRI(cpuid);
	if (!linux_context_data[cpuid].sflags) {
		_rt_switch_to_real_time(cpuid);
			return 0;
	} 
	return 1;
}

int yalrt_tune_timer (unsigned long ns, int flags) {
	if (!flags & IPIPE_RESET_TIMER) {
	hz = 1000000000 / ns;
	linux_time = 0;
	linux_next = LATCH;
	latch = (CLOCK_TICK_RATE + hz/2) /hz;
	}
	if (hz == HZ) return 0; /* useless to call adeos */
	else return -1; //return ipipe_tune_timer(ns,flags);
	}

void user_error(int type, void * which){

	STATIC_TASK_DESCRIPTOR *this_std;
	MSG_DESCRIPTOR *msg;

	this_std = rqactv->nameptr;
	printk("Task %6s ",this_std->name);
	switch (type) {
	case MSG_ERR:
		printk("Message Error %p\n",which);
		msg = which;
		printk("This Task= %p, Msg Link= %p\n",rqactv,msg->link);
		break;
	case EXCH_ERR:
		printk("Exchange Error %p\n",which);
		break;
	case INT_ERR:
		printk("Interrupt Error\n");
		break;
	default:
		printk("Unknown Error\n");
		break;
	}
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

static void reqdqmsg (INT_EXCHANGE_LIST_PTR exch) {

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

static void cancel_delay (TASK_LIST_PTR task){

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
	
	if ( !(task->status && suspended) ) {
		if (task->priority < ready->priority){
			task->thread = ready;
			ready = task;
			}
		else {
			prtask = ready;
			nxtask = prtask->thread;
			while ((nxtask != NULL) && (nxtask->priority <= task->priority)){
				prtask = nxtask;
				nxtask = nxtask->thread;
				}
			task->thread = prtask->thread;
			prtask->thread = task;
			}
		}
	else {
		if (suspend_list_root.headPtr == NULL) {
			suspend_list_root.headPtr = task;
			}
		else {
			prtask = suspend_list_root.tailPtr;
			prtask->thread = task;
			}
		task->thread = NULL;
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
			ready = task->thread;
			}
		else {
			CurrTask = ready;
			while ((CurrTask->thread != task) && (CurrTask->thread != NULL)) {
				CurrTask = CurrTask->thread;
				}
			if (CurrTask->thread == task) {
				CurrTask->thread = task->thread;
				}
			}
		}
	else if (suspend_list_root.headPtr != NULL) {
		if (suspend_list_root.headPtr == task){
			suspend_list_root.headPtr = task->thread;
			if (task->thread == NULL) {
				suspend_list_root.tailPtr = (void *)&suspend_list_root.headPtr;
				}
			}
		else {
			CurrTask = suspend_list_root.headPtr;
			while ((CurrTask->thread != task)&&(CurrTask->thread != NULL)) {
				CurrTask = CurrTask->thread;
				}
			if (CurrTask->thread == task) {
				CurrTask->thread = task->thread;
				if (task->thread == NULL) {
					suspend_list_root.tailPtr = (void *)&suspend_list_root.headPtr;
					}
				}
			}
		}
	}

/*---------------- Creation Functions ------------*/

/*--------------------------------------------
 reqctsk:
 create a task from the supplied static task descriptor
 and make it ready.
 -------------------------------------------*/


void reqctsk(STATIC_TASK_DESCRIPTOR *std) {

//DECLARE MTAB(9) BYTE DATA(0FFH,0FEH,0FCH,0F8H,0F0H,0E0H,0C0H,80H,0);
	TASK_LIST_PTR td;
	int * taskstack;
	int mastermask,slavemask;

	reqndpwait();
	enter_region;
	td = std->task;
	td->nameptr = std;
	td->status = 0;
	td->priority = std->priority;
	td->exchange = std->exchange;
	if (std->taskndp){
		ndpinit();
		}
//	taskstack = std->sp;
	if (!(taskstack = kmalloc(std->stklen, GFP_KERNEL))) {
		td->marker = NULL; // protects against a deletion
		//printk("Task %6s: Unable to allocate required stack",std->name);
		exit_region(0);
		return /*-ENOMEM */;
	}
	td->marker = taskstack;
	memset (taskstack, unusedflag, std->stklen);
//	(char *)taskstack = unusedflag;
//	memmove ((char *)taskstack[1],(char *)taskstack, (std->stklen-1) );
//	memcpy ((char *)taskstack[1],(char *)taskstack, (std->stklen-1) );
	taskstack = taskstack + (std->stklen)/sizeof(int);
	*(taskstack -3) = (int)std->pc;
//	*(taskstack -3) = (int)rqrun;   no ds switch
	*(taskstack -2) = (int)std->ds;
	*(taskstack -1) = (int)std->pc;
	/* WARNING - next instruction reflects stack layout at the end of
	exit_region: 3 (stack defined here) + 3(number of popl's on exit) = 6 */
	td->sp = taskstack -13; // 3(here) + 3(exit_region) + 7(save_all_reg)
	if (td->priority > 128){
		td->master_mask = td->slave_mask = 0;
		}
	else {
		if (td->priority < 3){
			td->master_mask = 0xFF;
			td->slave_mask = 0;
			}
		else {
			slavemask = (((td->priority-3) & 0xF) >> 1) +1;
			mastermask = (((td->priority-3) >> 4)+1);
			td->master_mask = 0xff << mastermask;
			td->slave_mask =  0xFF << slavemask;
			}
		}
	enterlist(td);
	if (rq_task_head == NULL) {
		rq_task_head = rq_task_tail = td;
		}
	else {
		rq_task_tail->tasklink = td;
		rq_task_tail = td;
		}
	td->tasklink = NULL;

	exit_region(0);
	}

/*--------------------------------------------
 reqcxch:
 Create (i.e. initialize) the exchange supplied
 -------------------------------------------*/

//void reqcxch(EXCHANGE_LIST_PTR exch) {
void reqcxch(EXCHANGE_LIST_PTR exch,const char *name) {

	int namelen;
	
	exch->message_head = NULL;
	exch->task_head = NULL;
	exch->message_tail = (void *)exch;
	exch->task_tail = (void *)exch;
	memset(exch->name,0,EXCH_NAME_LEN);
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
	}

/*--------------------------------------------
 reqcmsg:
 Create (i.e. initialize) the message supplied
 -------------------------------------------*/

void reqcmsg(MSG_DESCRIPTOR * msg, int msgsize) {

	msg->length = msgsize;
	msg->link = (void *)rqactv;
	msg->type = 0x40; // just to avoid system defined types
#ifndef USE_FIFO
	msg->yltime = rqsystime;
#endif
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

	if (task->marker == NULL) return; //avoids useless errors

	reqndpwait();
	enter_region;

	if (task != &IdleTask) {
		if (task == rq_last_ndp_task) {rq_last_ndp_task = NULL;}
		if((task->status &(waiting | delayed))) {
			if (task->exchange != NULL) {
				exch = task->exchange;
				ctask = (TASK_DESCRIPTOR *)task->exchange;
				while ((ctask->thread != task)&(ctask->thread != NULL)) {
					ctask = ctask->thread;
					}
				if (ctask->thread == task) {
					if((ctask->thread = task->thread) == NULL) {
						exch->task_tail = ctask;
						}
					}
				}
			if (task->status & delayed){ cancel_delay (task);}
			}

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

		removelist(task);

		if (task == rqactv) {
			/* we can't cut the branch we're sitting on ! */
			task->status |= deleted;
		} else {
			kfree (task->marker);
			task->marker = NULL;
			}
		}
	exit_region(0);
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

	reqndpwait();
	enter_region;
	result = false;
	if ((exch->message_head == NULL) && (exch->task_head == NULL)){
		result = true;
		//printk("reqdxch: deleting %6s_EX\n",exch->name);
		}
	//else printk("reqdxch: can't delete %6s_EX\n",exch->name);
	if (result) {
		result = false;
		prev = rq_exchange_head;
		while (prev && prev->exchange_link != exch)
			prev = prev->exchange_link;
		if (prev) {
			//printk("reqdxch: %6s_EX found\n",exch->name);
			curr = prev->exchange_link;
			prev->exchange_link = curr->exchange_link;
			if (rq_exchange_tail == curr) rq_exchange_tail = prev;
			result = true;
			}
		}
	exit_region(0);
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

	reqndpwait();
	enter_region;

	if (task != &IdleTask) {
		ctask = ready; // scan ready list
		while ((ctask != task) && (ctask != NULL)) {
			ctask = ctask->thread;
			}
		if (ctask == task) removelist(task);
		task->status |= suspended;
		if (ctask == task) enterlist(task);
		}
	exit_region(0);
	}

/*--------------------------------------------
 reqresm:
 resume a suspended task.

 Note - resuming a task which is not suspended is legal, but has no
 effect whatsoever. There isn't anything like a suspension counter
 or such stuff.
 -------------------------------------------*/

void reqresm (TASK_DESCRIPTOR *task){
	TASK_DESCRIPTOR *ctask;

	reqndpwait();
	enter_region;

	ctask = suspend_list_root.headPtr; // scan suspend list
	while ((ctask != task) && (ctask != NULL)) {
		ctask = ctask->thread;
		}
	if (ctask == task) removelist(task);
	task->status &= ~suspended;
	if (ctask == task) enterlist(task);
	exit_region(0);
	}
#define FreezeBake
#ifdef FreezeBake
/*-------------  Suspend / Resume Full system --------------------*/

/*--------------------------------------------
 reqfreeze:

 Freezes yalrt and give control back to Linux. Handy for debug
 or prior to termination, without worrying about dependencies.
 exchange deletion check
 -------------------------------------------*/

void reqfreeze (void) {
	int i;
	unsigned long flags;

	if (rqfrozen) return; // just once
	enter_region; /* no context switch allowed from now on */
	flags = ipipe_critical_enter(NULL); /* we can't be interrupted */
	for (i=0;i<NR_GLOBAL_IRQ;i++){
		if (rqlintex[i].assigned){
			if (i == TIMER_IRQ) {
				yalrt_tune_timer(0,IPIPE_RESET_TIMER);
				}
			ipipe_virtualize_irq(&yalrt_domain,
				i,
				NULL,
				NULL,
				NULL,
				IPIPE_PASS_MASK);
			}
		}
	rqfrozen = true;
	ipipe_critical_exit(flags); /* no more danger */
	saved_ready = ready;
	saved_rqactv = rqactv; // supposing we want to resume later
	printk("Yalrt nucleus frozen\n");
	ready = &IdleTask;
	exit_region(0);
	}

/*--------------------------------------------
 reqbake:

 Bakes a deep frozen yalrt and resumes operation.

 -------------------------------------------*/

void reqbake (void) {
	int i,irq;
	unsigned long flags;

	if (!rqfrozen) return; // don't kid
	enter_region; /* no context switch until we're done */
	flags = ipipe_critical_enter(NULL); /* we can't be interrupted */
	for (i=0;i<NR_GLOBAL_IRQ;i++){
		if (rqlintex[i].assigned){
			if (i == TIMER_8254_IRQ) {
				yalrt_tune_timer(rqsystick,0);
				irq = rqlintex[i].irq;
				ipipe_virtualize_irq(&yalrt_domain,irq,
					rqlintex[i].yalrt_handler,
					NULL,
					NULL,
					IPIPE_HANDLE_MASK | IPIPE_PASS_MASK);
				}
			else {
				ipipe_virtualize_irq(&yalrt_domain,i,
					rqlintex[i].yalrt_handler,
					NULL,
					NULL,
					IPIPE_HANDLE_MASK | IPIPE_PASS_MASK);
				}
			}
		}
	rqfrozen = false;
	ready = saved_ready;
	rqactv = saved_rqactv; // supposing we want to resume later
	ipipe_critical_exit(flags); /* ready to go */
	printk("Yalrt nucleus restored\n");
	exit_region(0);
	}

#endif /* FreezeBake */
/*------------------ THE REAL STUFF - THE SCHEDULER -------*/

/*--------------------------------------------
 exit_region:
 whenever we get out of the protected region, the currently active task
 may be no more at the top of the ready list, so we must reschedule
 SYSTEM INTERNAL USAGE ONLY
 -------------------------------------------*/

/* most local variables can be static, because exit_region avoid reentrancy
   by means of the "critical_region_flag" which is so called for
   historical reasons. Actually it is not a flag but a recursion counter */

#ifdef yalrtdbg
//static TASK_LIST_PTR	td;
#endif

void exit_region(int FromInt) {

struct registers {
		long flags;
		void * stack;
		};
INT_EXCHANGE_LIST_PTR Q_int_msg;
//TASK_LIST_PTR waiting_task;
TASK_DESCRIPTOR *waiting_task;
MSG_DESCRIPTOR *tail_msg;
static struct registers cpu;
	
/*----------------------------------------------------------
	See rtai/base/sched/sched.c for domain switching
	and ipipe_set_foreign_stack !!
-----------------------------------------------------------*/ 
	//printk("exit_region:");
	//printdomain();
	/* if (ipipe_current_domain != &yalrt_domain) {
		//printk("Trigger sysreq virq\n");
		ipipe_trigger_irq(yalrt_sysreq_virq);
		return;
		}*/ 
	if (critical_region_flag != 1) {
		if (queue_index > max_queue){max_queue = queue_index;};
		if (critical_region_flag > 100){
			user_error(INT_ERR,&queue_index);
			rqfreeze();
			}
		critical_region_flag--;
		return;
		}
	if (FromInt) {
		rqactv->status |= maskint;
		}
	while (queue_index) {
		Q_int_msg = isnd_queue[--queue_index];
		ipipe_stall_pipeline_head();
		Q_int_msg->type = int_type;
#ifndef USE_FIFO
		Q_int_msg->yltime = rqsystime;
#endif
		if (Q_int_msg->task_head == NULL) {
			if (Q_int_msg->link == (void*)&Q_int_msg->link) {
				tail_msg = Q_int_msg->message_tail;
				Q_int_msg->message_tail = (void *)&Q_int_msg->link;
				tail_msg->link = (void *)&Q_int_msg->link;
				Q_int_msg->link = NULL;
				Q_int_msg->qint = true;
				}
			else {
				Q_int_msg->type = missed_int_type;
				}
			}
		else {
			tail_msg =  (MSG_DESCRIPTOR *)&Q_int_msg->link;
			waiting_task = Q_int_msg->task_head;
			Q_int_msg->task_head = waiting_task->thread;
			if (waiting_task->thread == NULL) {
				Q_int_msg->task_tail = (void *)Q_int_msg;
				}
			if ((waiting_task->status = (waiting_task->status & ~waiting)) & delayed){
			 cancel_delay((TASK_LIST_PTR)waiting_task);
			 }
//			printk("queueIndex %d, Q_int_msg %p", queue_index,Q_int_msg);
//			printk("waiting_task =%p\n",waiting_task);
//			(TASK_DESCRIPTOR *)waiting_task->message = tail_msg->link = tail_msg;
			waiting_task->message = tail_msg->link = tail_msg;
			enterlist((TASK_LIST_PTR)waiting_task);
			}
		ipipe_unstall_pipeline_head();
		} /*  WHILE QUEUE_INDEX */
	if (ready != rqactv ) { /* context switch */
		if (rqactv == &IdleTask ) {
			switch_to_yalrt();
			}
		save_all_reg();
		get_stack_pointer(&rqactv->sp);
		set_stack_pointer(&ready->sp);
		restore_all_reg();
		/* now we can safely free the stack of a task deleting itself */
		if ((rqactv->status & deleted) == deleted){
			kfree (rqactv->marker);
			rqactv->marker = NULL;
			}
		rqactv = ready;
/*		IF (rqactv.STATUS AND ndp_task) <> 0 then {
		*/	/* here ndp support can be added, if required */
		/*	}	*/
#ifdef yalrtdbg
		task_log[History].task = rqactv;
		task_log[History].time = rqsystime;
		History++;
		History  &= (Task_log_size-1);
		//td = ready;
#endif
		}
	if  ((rqactv->status & maskint) == 0) {
		// hard_sti();
		if (rqactv == &IdleTask) {
				  switch_to_linux();
				  }
		critical_region_flag = 0;
		return;
		}
	else {
		rqactv->status &= ~maskint;
		critical_region_flag = 0;
		return;
		}
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

	if (rqfrozen) return; // handlers could do it
	reqndpwait();
	enter_region;
	if ((msg->link != (void *)rqactv)&&(msg->link != (void *)msg)){
		user_error(MSG_ERR, (void*)msg);
		exit_region(0);
		rqfreeze();
		return; // useless but...
		}
	if (exch->task_head == NULL ){
		if (exch->task_tail != (void *) exch) {
			user_error(EXCH_ERR,(void *)exch);
			exit_region(0);
			rqfreeze();
			return; // useless but...
			}
		previous_message = exch->message_tail;
		previous_message->link = msg;
		exch ->message_tail = msg;
		msg->link = NULL;
		}
	else {
		if ((exch->message_head != NULL ) || (exch->message_tail != (void *) exch)) {
			user_error(EXCH_ERR,(void *)exch);
			exit_region(0);
			rqfreeze();
			return; // useless but...
			}
		task = exch->task_head;
		if ((exch->task_head = task->thread) == NULL) {
			exch->task_tail = (void *) exch;
			}
		task->status &= ~waiting;
		if (task->status && delayed) {  // delayed
			cancel_delay(task);
			}
		enterlist(task);
		msg->link = (void *)task;
		task->message = msg;
		}
	exit_region(0);
	}

/*--------------------------------------------
 reqacpt:
 fetch message from an exchange and make it
 available to currently active task - if no message
 is available return NULL
 -------------------------------------------*/

MSG_DESCRIPTOR * reqacpt (EXCHANGE_LIST_PTR exch) {
	reqndpwait();
	enter_region;
	rqactv->exchange = exch;
	rqactv->message = NULL;
	if (exch->message_head != NULL){ reqdqmsg((INT_EXCHANGE_LIST_PTR)exch); }
	exit_region(0);
	return rqactv->message;
	}

/*--------------------------------------------
 reqwait:
 Wait for a message at an exchange. If no message is available
 the requesting task is put to wait either forever, or for the requested
 amount of time. If the time expires without a message being available
 the delay task will handle that.
 -------------------------------------------*/

	static TASK_LIST_PTR tail_task;
	static TASK_LIST_PTR delay_task;

MSG_DESCRIPTOR *reqwait(EXCHANGE_LIST_PTR exch, unsigned short delay){

	unsigned short t_delay;

	reqndpwait();
	enter_region;

	rqactv->exchange = exch;
	if (exch->message_head == NULL ) {
		ready = rqactv->thread;
		rqactv->thread = NULL;
		tail_task = exch->task_tail;
		tail_task->thread = rqactv;
		exch->task_tail = rqactv;
		if (delay) {
			rqactv->status |= delayed;
			rqactv->delay = delay;
			delay_task = &rq_delay_list_head;
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
			reqelvl(TIMER_IRQ);
			}
		if (!(rqactv->status & delayed)) {
			rqactv->status |= waiting;
			}
		}
	else {
		reqdqmsg((INT_EXCHANGE_LIST_PTR)exch);
		}
	// hard_sti();
	exit_region(0);
	if (rqactv->message == NULL) rqfreeze();
	return rqactv->message;
	}
/*----------------timer related functions--------------*/

INT_EXCHANGE_DESCRIPTOR rq_delay_exchange = {
	NULL,
	(MSG_DESCRIPTOR *)&rq_delay_exchange,
	NULL,
	(TASK_DESCRIPTOR *)&rq_delay_exchange,
	NULL,
	"DELYEX",
	0,
	TIMER_IRQ,
	NULL,
	NULL,
	int_exchange_length,
	int_type,
//	0,
	TIMER_8254_IRQ,
	0,
	0};
/*--------------------------------------------
 yalrt_timer_handler
 The timer interupt handler. It handles Linux timer
 following RTAI's scheme, and then performs yalrt service.
 It increments system timer and checks whether we have
 reached the proper count for the next task in the delay list.
 If so it just queues the interrupt message in the isnd queue,
 and leaves the delay task to take over.
---------------------------------------------*/

static void yalrt_timer_handler(unsigned int irq, void *cookie) {
	TASK_DESCRIPTOR *waiting_task;
	int thiscpu;
#ifdef SSND_SUPPORT
 int sendint = false;
#endif
//#define DEBUG_TIMER
	linux_time += latch;
	if (linux_time >= linux_next){
		linux_next +=LATCH;
#ifndef DEBUG_TIMER
		ipipe_propagate_irq(TIMER_IRQ);
#endif
		}
	/* if (++linux_time >= Ratio) {
		adeos_propagate_irq(TIMER_8254_IRQ);
		linux_time = 0;
		} */

	++rqsystime; // bump system ticks
	thiscpu = ipipe_processor_id();
	systime[thiscpu]++;
#ifdef DEBUG_TIMER
	ipipe_propagate_irq(TIMER_IRQ);
	return; // !!!! debug
#endif
	if (!rqlintex[TIMER_8254_IRQ].assigned) {
		return ;
		}
	waiting_task = rq_delay_list_head.next;
#ifdef SSND_SUPPORT
	if (queue_index) sendint = true;
	if (waiting_task->delay != 0xFFFF) {
		if (!waiting_task->delay){
			isnd_queue[queue_index++] = &rq_delay_exchange;
			sendint = true;
			}
		else if (!--waiting_task->delay){
			isnd_queue[queue_index++] = &rq_delay_exchange;
			sendint = true;
			}
		}
	if (sendint) {
//		hard_sti(); /* give a chance to other interrupts */
		enter_region;
//		hard_cli(); /* but just so */
		exit_region(1); /* �coerente con RTAI, ma �giusto?*/
		}
#else
	if (waiting_task->delay != 0xFFFF) {
		if (!waiting_task->delay){
			isnd_queue[queue_index++] = &rq_delay_exchange;
//			hard_sti(); /* give a chance to other interrupts */
			enter_region;
//			hard_cli(); /* but just so */
			exit_region(1); /* �coerente con RTAI, ma �giusto?*/
			}
		else if (!--waiting_task->delay){
			isnd_queue[queue_index++] = &rq_delay_exchange;
//			hard_sti(); /* give a chance to other interrupts */
			enter_region;
//			hard_cli(); /* but just so */
			exit_region(1); /* �coerente con RTAI, ma �giusto?*/
			}
		}
#endif
	}


/*--------------------------------------------
 rqdelaytask:
 It handles the "slow" timer interrupt service, when needed:
 it removes the appropriate task(s) from the delay list,
 it removes them from the exchange queue where they were waiting and
 puts them on the ready list.
 -------------------------------------------*/

TASK_DESCRIPTOR rqdelaytaskTd;

STATIC_TASK_DESCRIPTOR rqdelaytaskStd = {
	"*DELAY",
	&rqdelaytask,
	NULL,
	8192,
	0,
	18,
	NULL,
	&rqdelaytaskTd,
	0};

static void rqdelaytask(void) {

	TASK_LIST_PTR delay_task;
	TASK_LIST_PTR curr_task;
	EXCHANGE_DESCRIPTOR *task_exch;
	MSG_DESCRIPTOR *t_msg;

	 //printk("rqdelaytask initialize\n");
	 forever {
		t_msg = reqwait((EXCHANGE_DESCRIPTOR *)&rq_delay_exchange, 0);
		reqndpwait();
		enter_region;
		delay_task = rq_delay_list_head.next;
		while (delay_task->delay == 0) {
			task_exch = delay_task->exchange;
			curr_task = (TASK_DESCRIPTOR *)delay_task->exchange;
/* Subtle: the first time curr_task->thread actually means task_exch->next
because of the layout of the exchange and task structure!! */
			while (curr_task->thread != delay_task) {
				curr_task = curr_task->thread;
				}
			if ( (curr_task->thread = delay_task->thread) == 0 ) {
				task_exch->task_tail = curr_task;
				}
			cancel_delay(delay_task); /* questo fa rqdlvl se serve */
//			printk("Switching to TASK %p",&delay_task);
			enterlist(delay_task);
			delay_task->message = (MSG_DESCRIPTOR *)&time_out_message;
			delay_task = rq_delay_list_head.next;
//			delay_task->delay = 0xFFFF;
			}
		exit_region(0);
		}
	}

/*----------------interrupt related functions--------------*/

/*--------------------------------------------
 reqisnd:
 It is used by an interrupt handler to append a message to the
 appropriate interrupt exchange, so that interrupt related tasks
 can perform their action. It is also used by the default handler
 -------------------------------------------*/
void reqisnd (INT_EXCHANGE_DESCRIPTOR *int_ex) {

#ifdef yalrtdbg
	int_log[History_i].task = rqactv;
	int_log[History_i].time = rqsystime;
	int_log[History_i].level= int_ex->level;
	History_i++ ;
	History_i &= (Int_log_size-1);
#endif
	// if we use Adeos, we don't need ack here
	if ((void *)int_ex->link == (void *)&int_ex->link){
		isnd_queue[queue_index++] = int_ex;
		}
	else { int_ex-> type = missed_int_type; }
//	hard_sti
	ipipe_control_irq(&yalrt_domain,int_ex->level,0,IPIPE_ENABLE_MASK);
	enter_region;
//	hard_cli
	exit_region(1);
	}

#ifdef SSND_SUPPORT
/*--------------------------------------------
 reqssnd:
 It is used by an srq handler to append a message to the
 appropriate interrupt exchange, so that interrupt related tasks
 can perform their action. It is similar to reqisnd, but it relies
 on the timer handler to activate the interrupt.
 -------------------------------------------*/
void reqssnd (INT_EXCHANGE_DESCRIPTOR *int_ex) {
	unsigned long flags;

	enter_region;
#ifdef yalrtdbg
	int_log[History_i].task = rqactv;
	int_log[History_i].time = rqsystime;
	int_log[History_i].level= int_ex->level;
	History_i++ ;
	History_i &= (Int_log_size-1);
#endif

	flags = ipipe_critical_enter(NULL);
	if ((void *)int_ex->link == (void *)&int_ex->link){
		isnd_queue[queue_index++] = int_ex; // should be atomic?
		}
	else { int_ex-> type = missed_int_type; }
	ipipe_critical_exit(flags);
	exit_region(1);
	}
#endif

/*--------------------------------------------
 reqsxx:
 The default interrupt handler. When no interrupt handler
 is required, but an interrupt task will do.
 -------------------------------------------*/

static void reqsxx(unsigned int irq, void *cookie) {
	ipipe_control_irq(&yalrt_domain,irq,0,IPIPE_ENABLE_MASK);
	if (rqlintex[irq].assigned) reqisnd(&rqlintex[irq]);
	else ipipe_propagate_irq(irq);
	return;
	}

/*--------------------------------------------
 reqendi:
 It is used by an interrupt handler to notify that
 it's done, without activating a task
 -------------------------------------------*/
void reqendi (INT_EXCHANGE_DESCRIPTOR *int_ex) {
int irq;
#ifdef yalrtdbg
	int_log[History_i].task = rqactv;
	int_log[History_i].time = rqsystime;
	int_log[History_i].level= int_ex->level;
	History_i++ ;
	History_i &= (Int_log_size-1);
#endif
	// if we use RTAI dispatcher, we don't need ack here
	irq = int_ex->level;
	ipipe_control_irq(&yalrt_domain,irq,0,IPIPE_ENABLE_MASK);
	}

/*--------------------------------------------
 reqelvl:
 it enables for yalrt usage a given interrupt
 from yalrt it is legal to call it more than once,
 so a check on assigned is performed
 INT 0 is used for timer, even if it's an APIC timer
 -------------------------------------------*/
void reqelvl(int irq){
	INT_EXCHANGE_DESCRIPTOR *int_ex;
	int linux_irq;
	linux_irq = irq;
	if (irq == TIMER_IRQ) {
		irq = TIMER_8254_IRQ;
		}
	//printk("ELVL -irq %d ipipe_current_domain = %p\n",irq,ipipe_current_domain);
	int_ex = &rqlintex[irq];
	if (!int_ex->assigned){
		int_ex->assigned = 1;
	// we must set-up the interrupt section of the exchange
		if (int_ex->message_head == NULL){
			int_ex->link = (void *)&int_ex->link;
			int_ex->type = int_type;
			int_ex->qint = 0;
			}
		int_ex->length = int_exchange_length;
		int_ex->irq = linux_irq;
		int_ex->level = irq;
		int_ex->fill = 0;
		if (irq == TIMER_8254_IRQ) {
			yalrt_tune_timer(rqsystick,0);
			if (ipipe_virtualize_irq(&yalrt_domain,linux_irq,
				rqlintex[irq].yalrt_handler,
				NULL,
				NULL,
				IPIPE_HANDLE_MASK | IPIPE_PASS_MASK)) printk("ELVL Err Virt %d TIMER_IRQ \n",linux_irq);
			/*if (ipipe_control_irq(&yalrt_domain,irq,0,IPIPE_ENABLE_MASK))
			  printk("ELVL Err Control enable %d\n",irq);
			if (ipipe_control_irq(&yalrt_domain,irq,0,IPIPE_HANDLE_MASK))
			  printk("ELVL Err Control handle %d\n",irq);*/
			}
		else {
			if (ipipe_virtualize_irq(&yalrt_domain,irq,
				rqlintex[irq].yalrt_handler,
				NULL,
				NULL,
				(IPIPE_HANDLE_MASK | IPIPE_PASS_MASK)
				/* IPIPE_DEFAULT_MASK*/)) printk("ELVL Err Virt %d \n",irq);
			/*if (ipipe_control_irq(&yalrt_domain,irq,0,IPIPE_ENABLE_MASK))
			  printk("ELVL Err Control enable %d\n",irq);
			if (ipipe_control_irq(&yalrt_domain,irq,0,IPIPE_HANDLE_MASK))
			  printk("ELVL Err Control handle %d\n",irq);*/
			}
		}
	}

/*--------------------------------------------
 reqdlvl:
 it disables for yalrt usage a given interrupt
 if already disabled its a no-operation
 -------------------------------------------*/
void reqdlvl(int irq){
	if (rqlintex[irq].assigned){
		if (irq == TIMER_8254_IRQ) {
			yalrt_tune_timer(0,IPIPE_RESET_TIMER);
			ipipe_virtualize_irq(&yalrt_domain,rqlintex[irq].irq,
				NULL,
				NULL,
				NULL,
				IPIPE_PASS_MASK);
			}
		ipipe_virtualize_irq(&yalrt_domain,irq,
			NULL,
			NULL,
			NULL,
			IPIPE_PASS_MASK);
		rqlintex[irq].assigned = 0;
		}
	}

/*--------------------------------------------
 reqsetv:
 it assigns a non-default handler to an interrupt
 if a default handler had been previously assigned,
 it does the right things. An reqelvl is required
 to activate the new handler
 -------------------------------------------*/
void reqsetv(int irq,void (*handler)(void)) {
	if (rqlintex[irq].assigned) {
		rqlintex[irq].assigned = 0;
		}
	if (handler == NULL) {
		handler = (void *)&reqsxx;
		}
	rqlintex[irq].yalrt_handler = handler;
	}
/*----------------- Proc Filesystem ------------*/
#ifdef CONFIG_PROC_FS

struct proc_dir_entry *yalrt_proc_root = NULL;
static struct proc_dir_entry *yalrt_proc_root_nucleus;
static struct proc_dir_entry *yalrt_proc_root_tasks;
static struct proc_dir_entry *yalrt_proc_root_stacks;
static struct proc_dir_entry *yalrt_proc_root_exchanges;

static int yalrt_read_nucleus(char* buf, char** start, off_t offset, int len, int *eof, void *data) {
	int i,t;
	
	TASK_LIST_PTR this_task;
	STATIC_TASK_DESCRIPTOR *this_std;

	i = 0;
	if (rqfrozen) {
		len = sprintf(buf, "\n**********  YALRT - Nucleus frozen  **********\n\n" );
		if (len > LIMIT) {
			return(len);
			}
		}
	else {
	len = sprintf(buf, "\n**********  YALRT - Nucleus status  **********\n\n" );
		if (len > LIMIT) {
			return(len);
			}
		}
	
	len += sprintf(buf + len, "----- Enabled Interrupts (w. handler) -----\n" );
	if (len > LIMIT) {
		return(len);
		}
	
	for (i=0;i<NR_GLOBAL_IRQ;i++){
		if ( rqlintex[i].assigned) {
			if ((len += sprintf(buf + len, " %d", i)) > LIMIT) {
				return(len);
				}
			if (rqlintex[i].yalrt_handler != &reqsxx) {
				if ((len += sprintf(buf + len, "(h)")) > LIMIT) {
					return(len);
					}
				}
			}
		}
	
	len += sprintf(buf + len, "\n\n----- Ready Tasks -----\n" );
	if (len > LIMIT) {
		return(len);
		}
	
	this_task = rqfrozen ? saved_ready : ready;
	while (this_task != NULL) {
		this_std = this_task->nameptr;
		if (this_std == NULL) {
			len += sprintf(buf + len, "Ooops, Ready list Std, null Pointer\n");
			return (len);
			}
		len += sprintf(buf + len, "%6s ",this_std->name);
		len += sprintf(buf + len, "st = %2x ",this_task->status);
		len += sprintf(buf + len, "last excg = %6s_EX ",this_task->exchange->name);
		len += sprintf(buf + len, "last msg = %p ",this_task->message);
		len += sprintf(buf + len, "pri = %d \n",this_task->priority);
		if (len > LIMIT) {
			return(len);
			}
		this_task = this_task->thread;
		}
	
	len += sprintf(buf + len, "\n\n----- Delayed Tasks -----\n" );
	if (len > LIMIT) {
		return(len);
		}
	t=0;
	this_task = rq_delay_list_head.next;
		if (this_task == NULL) {
			len += sprintf(buf + len, "Ooops,  Delay list Td null Pointer\n");
			return (len);
			}
	while (this_task->delay != 0xFFFF) {
		this_std = this_task->nameptr;
		if (this_std == NULL) {
			len += sprintf(buf + len, "Ooops, delay list Std, null Pointer\n");
			return (len);
			}
		t += this_task->delay;
		len += sprintf(buf + len, "%6s ",this_std->name);
		len += sprintf(buf + len, "time left = %d \n",t);
		if (len > LIMIT) {
			return(len);
			}
		this_task = this_task->next;
		}
	len += sprintf(buf + len, "\nUptime = %d ms CPU0 = %ld CPU1 = %ld\n",rqsystime,systime[0],systime[1]);
	return (len);
	}

static int yalrt_read_stacks(char* buf, char** start, off_t offset, int len, int *eof, void *data) {
	int t;
	
	TASK_LIST_PTR this_task;
	STATIC_TASK_DESCRIPTOR *this_std;

	len = sprintf(buf, "\n----- Stack Usage -----\n" );
	if (len > LIMIT) {
		return(len);
		}
	
	this_task = rq_task_head;
	while (this_task != NULL) {
		this_std = this_task->nameptr;
		if (this_std == NULL) {
			len += sprintf(buf + len, "Ooops, Task list Std, null Pointer\n");
			return (len);
			}
		len += sprintf(buf + len, "%6s ",this_std->name);
		len += sprintf(buf + len, "st = %2x ",this_task->status);
		len += sprintf(buf + len, "stack = %5d ",this_std->stklen);
		
		//t = memchr(this_task->marker,unusedflag,this_std->stklen) - this_task->marker;

		t = memrchr(this_task->marker,unusedflag,this_std->stklen) - this_task->marker;
		len += sprintf(buf + len, "unused = %d\n",t);
		if (len > LIMIT) {
			return(len);
			}
		
		this_task = this_task->tasklink;
		}
	
	return (len);
	}
	
static int yalrt_read_tasks(char* buf, char** start, off_t offset, int len, int *eof, void *data) {
	
	TASK_LIST_PTR this_task;
	STATIC_TASK_DESCRIPTOR *this_std;

	len = sprintf(buf, "\n-------- Tasks --------\n" );
	if (len > LIMIT) {
		return(len);
		}
	
	this_task = rq_task_head;
	while (this_task != NULL) {
		this_std = this_task->nameptr;
		if (this_std == NULL) {
			len += sprintf(buf + len, "Ooops, Task list Std, null Pointer\n");
			return (len);
			}
		len += sprintf(buf + len, "%6s ",this_std->name);
		len += sprintf(buf + len, "st = %2x ",this_task->status);
		len += sprintf(buf + len, "last excg = %6s_EX ",this_task->exchange->name);
		len += sprintf(buf + len, "last msg = %p ",this_task->message);
		len += sprintf(buf + len, "pri = %d \n",this_task->priority);
		if (len > LIMIT) {
			return(len);
			}
		this_task = this_task->tasklink;
		}
	
	return (len);
	}
	
static int yalrt_read_exchanges(char* buf, char** start, off_t offset, int len, int *eof, void *data) {
	int t=0;
	
	EXCHANGE_LIST_PTR this_exch;
	MSG_DESCRIPTOR *this_msg;
	TASK_LIST_PTR this_task;
	STATIC_TASK_DESCRIPTOR *this_std;

	len = sprintf(buf, "\n-------- Exchanges --------\n" );
	if (len > LIMIT) {
		return(len);
		}
	
	this_exch = rq_exchange_head;
	while (this_exch != NULL) {
		this_msg = this_exch->message_head;
		this_task = this_exch->task_head;
		
		if ((this_msg != NULL)||(this_task != NULL)|| true) {
			len += sprintf(buf + len, "\nExchg: %6s_EX ",this_exch->name);
			if (len > LIMIT) {
				return(len);
				}
			}
		if (this_msg != NULL) {
			t = 0;
			len += sprintf(buf + len, "Queued Messages: ");
			if (len > LIMIT) {
				return(len);
				}
			}
		while (this_msg != NULL) {
			if (++t > 3) {
				len += sprintf(buf + len, "...");
				if (len > LIMIT) {
					return(len);
					}
				break;
				}
			else len += sprintf(buf + len, " %p type= %2X",this_msg,this_msg->type);
			if (len > LIMIT) {
				return(len);
				}
			this_msg = this_msg->link;
			}
		
		if (this_task != NULL) {
			t = 0;
			len += sprintf(buf + len, "Queued Tasks: ");
			}
		while (this_task != NULL) {
			this_std = this_task->nameptr;
			if (this_std == NULL) {
				len += sprintf(buf + len, "Ooops, Task list Std, null Pointer\n");
				return (len);
				}
			if (++t > 3) {
				len += sprintf(buf + len, "...");
				if (len > LIMIT) {
					return(len);
					}
				break;
				}
			else len += sprintf(buf + len, "%6s ",this_std->name);
			if (len > LIMIT) {
				return(len);
				}
			this_task = this_task->thread;
			}
		this_exch = this_exch->exchange_link;
		}
	
	len += sprintf(buf + len, "\n");
	return (len);
	}
	
#ifdef MODULE_kernel_2
void yalrt_modcount(struct inode *inode, int fill)
{
	if(fill)
		MOD_INC_USE_COUNT;
	else
		MOD_DEC_USE_COUNT;
}
#endif

static int yalrt_proc_register(void)
{
	yalrt_proc_root = create_proc_entry("yalrt", S_IFDIR, 0);

	if (!yalrt_proc_root) {
	printk(KERN_ERR "Unable to initialize /proc/yalrt\n");
	return -1;
	}
#ifdef MODULE_kernel_2
	yalrt_proc_root->fill_inode = yalrt_modcount;
#endif


	yalrt_proc_root_nucleus = create_proc_entry("nucleus", 0, yalrt_proc_root);
	yalrt_proc_root_nucleus->read_proc = yalrt_read_nucleus;

	yalrt_proc_root_tasks = create_proc_entry("tasks", 0, yalrt_proc_root);
	yalrt_proc_root_tasks->read_proc = yalrt_read_tasks;

	yalrt_proc_root_stacks = create_proc_entry("stacks", 0, yalrt_proc_root);
	yalrt_proc_root_stacks->read_proc = yalrt_read_stacks;

	yalrt_proc_root_exchanges = create_proc_entry("exchanges", 0, yalrt_proc_root);
	yalrt_proc_root_exchanges->read_proc = yalrt_read_exchanges;


	return(0);
}	/* End function - yalrt_proc_register */


static void yalrt_proc_unregister(void)
{
	remove_proc_entry("exchanges",yalrt_proc_root);
	remove_proc_entry("stacks",yalrt_proc_root);
	remove_proc_entry("tasks",yalrt_proc_root);
	remove_proc_entry("nucleus",yalrt_proc_root);
	remove_proc_entry("yalrt",0);
}	/* End function - yalrt_proc_unregister */


#endif

/*-----------SETUP and CLEANUP stuff -----------*/

/* GUARDARE CODICE di switch_rtai_tasks  in /base/sched/sched.c */

void switch_to_yalrt(void) {
	//printdomain();
	int cpuid = ipipe_processor_id();
	//printk("Switch to Yalrt CPU%d\n",cpuid);
	rt_switch_to_real_time(cpuid);
	}

void switch_to_linux(void) {
	int cpuid = ipipe_processor_id();
	//printdomain();
	//printk("Switch to Linux CPU%d\n",cpuid);
	rt_switch_to_linux(cpuid);
	}

void switch_domain(unsigned virq){
	//printdomain();
	printk("Virq handler: virq=%d\n",virq);
	exit_region(0);
	}

/*--------------------------------------------
 reqnucl:
 It initializes the yalrt nucleus - Nothing fancy.
 Some useless zeroing made for clarity.
 -------------------------------------------*/
static void reqnucl(void){

	int irq, trapnr;
	char name [EXCH_NAME_LEN+1];

	if (true) {
		/* Trap all faults. */
/*		for (trapnr = 0; trapnr < HAL_NR_FAULTS; trapnr++)
		hal_catch_event(yalrt_domain,trapnr,(void *)yalrt_event_handler);*/
	
		yalrt_sysreq_virq = ipipe_alloc_virq();

		ipipe_virtualize_irq(ipipe_root_domain,
				  yalrt_sysreq_virq,
				  (void *)switch_domain,
				  NULL,
				  NULL,
				  IPIPE_HANDLE_MASK);

		reqndpwait();
		enter_region; /* protected from here until we're finished */

		ready = rqactv = IdleStd.task;
		rq_last_ndp_task =
		suspend_list_root.headPtr = NULL;
		suspend_list_root.tailPtr = (void *)&suspend_list_root;

		IdleTask.thread = NULL;
		IdleTask.priority = IdleStd.priority;
		IdleTask.nameptr = &IdleStd;
		IdleTask.status = 0;
		IdleTask.marker = 0;
		IdleTask.master_mask = IdleTask.slave_mask = 0;

		rq_delay_list_head.next = rq_delay_list_head.prev = (void*)&rq_delay_list_tail;
		rq_delay_list_head.thread = NULL;
		rq_delay_list_head.delay = 0;

		rq_delay_list_tail.next = rq_delay_list_tail.prev = (void*)&rq_delay_list_head;
		rq_delay_list_tail.thread = NULL;
		rq_delay_list_tail.delay = 0xFFFF;

		queue_index = 0;

		time_out_message.link = (MSG_DESCRIPTOR *)&time_out_message;
		time_out_message.length = sizeof(time_out_message);
		time_out_message.type = timeout_type;

		time_out_message.link = NULL; /* system message, can't be sent*/
		max_queue = 0;
		rq_exchange_head = rq_exchange_tail = NULL;

		for (irq=0;irq<NR_GLOBAL_IRQ;irq++){
			sprintf(name," RQL%2X",irq);
			reqcxch((EXCHANGE_LIST_PTR)&rqlintex[irq],name);
			rqlintex[irq].assigned = 0;
			rqlintex[irq].yalrt_handler = &reqsxx;
			}

		rqlintex[TIMER_8254_IRQ].yalrt_handler = &yalrt_timer_handler;
		reqcxch((EXCHANGE_LIST_PTR)&rq_delay_exchange,"DELAY");
		reqctsk(&rqdelaytaskStd);

		reqelvl(TIMER_IRQ); // activate timer
		StartTime = jiffies;
		systime[0] = 0;
		systime[1] = 0;
		//reqelvl(5); // prova

		exit_region(0);
		printk(KERN_INFO "YALRT: <%s> mounted.\n",YALRT_VERSION);
		//printk(KERN_INFO "YALRT: compiled with %s.\n", CONFIG_RTAI_COMPILER);
		printk("Start = %d\n",rqsystime);
		}
	/* this is the actual body of the idle task */
	forever break;
	}


static int __init yalrt_init(void) {

	int trapnr, yalrtinv = 0;
	struct ipipe_domain_attr  attr;

#ifdef CONFIG_PROC_FS
	yalrt_proc_register();
#endif
	printk(KERN_INFO "\nStarting Yalrt: CPU=%d Current Timer is: %d, Current div is: %ld\n",ipipe_processor_id(),HZ,LATCH);
	//printk(KERN_INFO "Module yalrt loaded\n");
	printk(KERN_INFO "IPIPE_NR_EVENTS = %d\n",IPIPE_NR_EVENTS);
	printk(KERN_INFO "NR_GLOBAL_IRQ = %d\n",NR_GLOBAL_IRQ);
	printk(KERN_INFO "IPIPE_SERVICE_IPI3 = %d\n",IPIPE_SERVICE_IPI3);
	printk(KERN_INFO "");
#ifdef CONFIG_X86_LOCAL_APIC
	printk(KERN_INFO "X86_LOCAL_APIC is configurated\n");
#endif
#ifdef CONFIG_X86_TSC
	printk(KERN_INFO "X86_TSC is configurated\n");
#endif 
#ifdef CONFIG_GENERIC_CLOCKEVENTS
	printk(KERN_INFO "GENERIC CLOCK EVENTS are configurated\n");
#else
	printk(KERN_INFO "GENERIC CLOCK EVENTS aren't configurated\n");
#endif
#ifdef IPIPE_NOSTACK_FLAG
	printk(KERN_INFO "IPIPE_NOSTACK_FLAG is configurated\n");
#else
	printk(KERN_INFO "IPIPE_NOSTACK_FLAG isn't configurated\n");
#endif
	/* for (yalrtinv = trapnr = 0; trapnr < IPIPE_NR_EVENTS; trapnr++) {
		if (ipipe_root_domain->evhand[trapnr]) {
			yalrtinv = 1;
			printk("EVENT %d INVALID.\n", trapnr);
		}
	}*/

	if (yalrtinv) {
		printk(KERN_ERR "YALRT: IPIPE IMMEDIATE EVENT DISPATCHING BROKEN.\n");
	}

	printk("YALRT: NUM ONLINE CPUS = %d\n",num_online_cpus());
	/*if (num_online_cpus() > RTAI_NR_CPUS) {
		printk("YALRT[hal]: RTAI CONFIGURED WITH LESS THAN NUM ONLINE CPUS.\n");
		yalrtinv = 1;
	}*/

#ifndef CONFIG_X86_TSC
	if (smp_num_cpus > 1) {
		printk("YALRT: MULTI PROCESSOR SEEN AS A 486, WONT WORK; CONFIGURE LINUX APPROPRIATELY.\n");
		yalrtinv = 1;
	}
#endif

#ifdef CONFIG_X86_LOCAL_APIC
//	if (!test_bit(X86_FEATURE_APIC, boot_cpu_data.x86_capability)) {
	if (!boot_cpu_has(X86_FEATURE_APIC)) {
		printk("YALRT[hal]: ERROR, LOCAL APIC CONFIGURED BUT NOT AVAILABLE/ENABLED.\n");
		yalrtinv = 1;
	}
#endif

	if (!(yalrt_sysreq_virq = ipipe_alloc_virq())) {
		printk(KERN_ERR "YALRT[hal]: NO VIRTUAL INTERRUPT AVAILABLE.\n");
		yalrtinv = 1;
	}

	if (yalrtinv) {
		return -1;
	}

	/* for (trapnr = 0; trapnr < NR_GLOBAL_IRQ; trapnr++) {
		yalrt_realtime_irq[trapnr].irq_ack = (void *)ipipe_root_domain->irqs[trapnr].acknowledge;
	}*/
/*	hal_virtualize_irq(ipipe_root_domain, yalrt_sysreq_virq, &rtai_lsrq_dispatcher, NULL, IPIPE_HANDLE_MASK);
	saved_hal_irq_handler = hal_irq_handler;
	hal_irq_handler = rtai_hirq_dispatcher;
*/
	ipipe_init_attr(&attr);
	attr.name	 = "YALRT";
	attr.domid	= YALRT_DOMAIN_ID;
	attr.entry	= (void *)reqnucl;
	attr.priority =  IPIPE_ROOT_PRIO+50;//IPIPE_HEAD_PRIORITY;
	yalrtinv = ipipe_register_domain(&yalrt_domain, &attr);
	if (yalrtinv) return -1;
	else return 0;
	}

static void __exit yalrt_exit(void){
	printk("Stop = %d (Jiffies=%d)\n",rqsystime,jiffies-StartTime);
	printk(KERN_INFO "\nYALRT: unmounting.\n");
	//ipipe_tune_timer(0,IPIPE_RESET_TIMER);
#ifdef CONFIG_PROC_FS
	yalrt_proc_unregister();
#endif
	if (yalrt_sysreq_virq)
		{
		ipipe_virtualize_irq(ipipe_root_domain,yalrt_sysreq_virq,NULL,NULL,NULL,0);
		ipipe_free_virq(yalrt_sysreq_virq);
		}
	ipipe_unregister_domain(&yalrt_domain);
	printk(KERN_INFO "Module yalrt unloaded\n");
	}


module_init(yalrt_init);
module_exit(yalrt_exit);

EXPORT_SYMBOL(rqsystick);
EXPORT_SYMBOL(rqsystime);
EXPORT_SYMBOL(rqactv);
EXPORT_SYMBOL(reqctsk);
EXPORT_SYMBOL(reqcxch);
EXPORT_SYMBOL(reqcmsg);
EXPORT_SYMBOL(reqdtsk);
EXPORT_SYMBOL(reqdxch);
EXPORT_SYMBOL(reqsusp);
EXPORT_SYMBOL(reqresm);
EXPORT_SYMBOL(reqfreeze);
EXPORT_SYMBOL(reqbake);
EXPORT_SYMBOL(reqsend);
EXPORT_SYMBOL(reqacpt);
EXPORT_SYMBOL(reqwait);
EXPORT_SYMBOL(reqelvl);
EXPORT_SYMBOL(reqdlvl);
EXPORT_SYMBOL(reqsetv);
EXPORT_SYMBOL(reqisnd);
EXPORT_SYMBOL(reqssnd);
EXPORT_SYMBOL(reqendi);
EXPORT_SYMBOL(rqlintex);
EXPORT_SYMBOL(exit_region);
EXPORT_SYMBOL(critical_region_flag);
EXPORT_SYMBOL(yalrt_proc_root);
EXPORT_SYMBOL(rq_exchange_head);
EXPORT_SYMBOL(rq_exchange_tail);
EXPORT_SYMBOL(rq_task_head);
EXPORT_SYMBOL(rq_task_tail);
