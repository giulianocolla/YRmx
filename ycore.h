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

/* Some useful constants */

/*--------------------------------------------
	 LSCHED lets Linux scheduler to take care
	  of running and suspendig tasks
--------------------------------------------- */
#define LSCHED

#define	true		0xff
#define false		0
#define forever		while (true)

/* The following define is for a special requirement: message compatibility
 with other remote processors, connected via high speed serial line which
 requires a one byte alignement. Sending messages through a serial line
 (even if high speed) )is so slow, that the efficiency gained using byte
 alignement greatly overcomes
 the loss in CPU efficiency. Moreover, the header part of the message, which
 is the only one the kernel deals with, is already word aligned, so the loss
 is only marginal. Comment out if you don't like it.
  */
#define RMX_COMPATIBLE

/*
* An exchange points to a FIFO linked list of messages and a FIFO linked list
*  of tasks.
*  When the list is empty, the head is a NULL pointer, while the tail points
*  to the head. This makes insertion and deletion very easy, without need to
*  test for special cases.
*  At any given moment only one of the two lists can be non-empty, as obvious.
*  All exchanges are linked together (Starting from rq_exchange_head) in a list, using
*  the exchange_link field, for ease of debugging.
*  Also the name field is useful only for debugging purposes.
*
*  When using Linux scheduler, a mutex ensure mutual exclusion when dealing
*  with exchange structures.
*/

#define EXCH_NAME_LEN 6

typedef struct exchange_descriptor  {
		struct msg_descriptor *message_head;
		struct msg_descriptor *message_tail;
		struct task_descriptor *task_head;
		struct task_descriptor *task_tail;
		struct exchange_descriptor *exchange_link;
		pthread_mutex_t mutex;
		unsigned char name[EXCH_NAME_LEN+1];
		} EXCHANGE_DESCRIPTOR;
		
#define exchange_descriptor_length (sizeof (EXCHANGE_DESCRIPTOR))

#define EXCH_INITIALIZER(nam) \
	{NULL,NULL,NULL,NULL,NULL,PTHREAD_MUTEX_INITIALIZER,nam}

typedef struct	int_exchange_descriptor {
		struct msg_descriptor *message_head;
		struct msg_descriptor *message_tail;
		struct task_descriptor *task_head;
		struct task_descriptor *task_tail;
		struct exchange_descriptor *exchangelink;
		pthread_mutex_t mutex;
		unsigned char	name[EXCH_NAME_LEN+1];
		unsigned char assigned;
		int irq;
		void *yalrt_handler;
		struct msg_descriptor *link;
		unsigned int length;
		unsigned int type;
		unsigned int yltime;
		unsigned char level;
		unsigned char qint;
		unsigned char fill;
		} INT_EXCHANGE_DESCRIPTOR;

#define int_exchange_length (sizeof(INT_EXCHANGE_DESCRIPTOR))

typedef struct int_msg_descriptor {
		struct msg_descriptor *link;
		unsigned int length;
		unsigned int type;
		unsigned int yltime;
		unsigned char level;
		unsigned char qint;
		unsigned char fill;
		} INT_MSG_DESCRIPTOR;

/*
* A message is composed of a system defined header, and (optionally) by a user
* defined part.
* The link field is used to support the FIFO list when a message is appended to
* an exchange. When the message is received by a task, the link points to the
* task descriptor, and establishes message ownership, to avoid share conflicts.
* Length represents the full length in bytes, user part included.
* Home Exchange is the exchange where the message may be sent to dispose of it:
* it helps implementing one or more message pools, in order to avoid timing
* uncertainties due to memory allocation
* Response Exchange is the Exchange where the Receiving Task should send a
* response if appropriate.
*/

#define msghdr \
		struct msg_descriptor *link; \
		unsigned int length; \
		unsigned int type;\
		unsigned int yltime; \
		struct exchange_descriptor *home_exchange; \
		struct exchange_descriptor *response_exchange

#ifdef RMX_COMPATIBLE
#pragma pack(1)
#endif

typedef struct	msg_descriptor {
		msghdr;
		unsigned char user_defined [1];
		} MSG_DESCRIPTOR;

// for the timeout message
struct sys_msg {
	struct msg_descriptor *link;
	unsigned int length;
	unsigned int type;
	unsigned int yltime;
	};

#ifdef RMX_COMPATIBLE
#pragma pack()
#endif

#define minmsglength  (sizeof(MSG_DESCRIPTOR)-1)

/*
* Those are the system defined Message types which should not be used for other
* purposes. System messages don't have a Response exchange, and they must not
* be sent anywhere by user tasks.
*
*/
#define int_type  1
#define missed_int_type  2
#define timeout_type  3

/*
* The static task descriptor is the structure passed to the create task system
* call to create a new task. It contains only information to initialize the
* task, to allocate the required resources, and to build the task descriptor
* structure, which is used during system operation. The name field is mainly
* used for debug.
*/

typedef struct static_task_descriptor {
				unsigned char	name[6];
				void 			*pc;
				int				*sp;
				unsigned int	stklen;
				int				*ds;
				unsigned char	priority;
				struct exchange_descriptor *exchange;
				struct task_descriptor *task;
				unsigned char	taskndp;
				} STATIC_TASK_DESCRIPTOR;

#define task_links \
	struct task_descriptor *next;	/*linkforward	 */\
	struct task_descriptor *prev;	/*linkback		 */\
	struct task_descriptor *link; /*link */\
	unsigned short delay;\
	struct timespec ts;

#define td_middle_part\
	struct exchange_descriptor *exchange; \
	sem_t			sema; \
	pthread_t		thread; \
	void			(*thread_start)(); \
	unsigned char	priority;\
	unsigned char	status;\
	struct static_task_descriptor *nameptr;

#define td_end_part \
	struct task_descriptor *tasklink;\
	unsigned char master_mask;\
	unsigned char slave_mask;

typedef struct task_descriptor {
	task_links;
	struct msg_descriptor *message;
	td_middle_part;
	td_end_part;
	} TASK_DESCRIPTOR;

#define STD_INITIALIZER(nam,strt,pri,td) \
	{nam,&strt,NULL,16384,0,pri,NULL,&td,0}

typedef struct {
		task_links;
		} DELAY_LIST_QUEUE;

#define task_descriptor_length  sizeof(TASK_DESCRIPTOR)

#define MAX_SAFE_STACK (8*1024) /* Default maximum stack safe to access
								without faulting - can be overriden with
								stklen */

#define	delayed		0x01
#define	suspended	0x02
#define	maskint		0x04
#define	helper		0x08
#define running		0x10
#define	needpost	0x20
#define deleted		0x40
#define waiting		0x80

#define bottomflag		0xc7c7
#define unusedflag		0xc7

typedef STATIC_TASK_DESCRIPTOR *STD_LIST_PTR;
typedef TASK_DESCRIPTOR *TASK_LIST_PTR;
typedef EXCHANGE_DESCRIPTOR *EXCHANGE_LIST_PTR;
typedef INT_EXCHANGE_DESCRIPTOR *INT_EXCHANGE_LIST_PTR;

typedef EXCHANGE_LIST_PTR *IET[];
typedef STD_LIST_PTR *ITT[];

extern int rqfrozen;
extern unsigned int reqsystime();
#define rqsystime reqsystime()

extern void reqctsk(STATIC_TASK_DESCRIPTOR *);
//extern void reqcxch(EXCHANGE_LIST_PTR);
extern void reqcxch(EXCHANGE_LIST_PTR, const char *);
extern void reqcmsg(MSG_DESCRIPTOR * , int );

extern void reqdtsk (TASK_DESCRIPTOR *);
extern int reqdxch (EXCHANGE_DESCRIPTOR *);

extern void reqsusp (TASK_DESCRIPTOR *);
extern void reqresm (TASK_DESCRIPTOR *);

extern void reqfreeze(void);
extern void reqbake(void);

extern void reqsend (EXCHANGE_LIST_PTR , MSG_DESCRIPTOR *);
extern MSG_DESCRIPTOR * reqacpt (EXCHANGE_LIST_PTR );
extern MSG_DESCRIPTOR *reqwait(EXCHANGE_LIST_PTR , unsigned short);

extern void reqelvl(int);
extern void reqdlvl(int);
extern void reqsetv(int irq,void (*handler)(void));

extern void exit_region(TASK_DESCRIPTOR *task);
extern int critical_region_flag;
#define enter_region

//#define exit_region(x) --critical_region_flag

extern TASK_LIST_PTR active_task(void);

#define handle_error(msg) \
	do { perror(msg); exit(EXIT_FAILURE); } while (0)

#define handle_error_en(en, msg) \
		do { errno = en; perror(msg); exit(EXIT_FAILURE); } while (0)

extern int no_print (const char *fmt, ...);

/* user interface calls */
#define MAX_TASK	256
#define MAX_EXCH	256

extern int reqstart (STD_LIST_PTR itt[],int ntask,EXCHANGE_LIST_PTR iet[],int nexch);

#define rqstart reqstart
#define rqctsk reqctsk
#define rqcxch reqcxch
#define rqcmsg reqcmsg

#define rqdtsk reqdtsk
#define rqdxch reqdxch

#define rqsusp reqsusp
#define rqresm reqresm

#define rqfreeze reqfreeze
#define rqbake reqbake

#define rqsend reqsend
#define rqacpt reqacpt
#define rqwait reqwait

#define rqelvl reqelvl
#define rqdlvl reqdlvl
#define rqsetv reqsetv

#define rqisnd reqisnd
#ifdef SSND_SUPPORT
#define rqssnd reqssnd
#endif
#define rqendi reqendi

extern EXCHANGE_LIST_PTR rq_exchange_head;
extern EXCHANGE_LIST_PTR rq_exchange_tail;
extern TASK_LIST_PTR rq_task_head;
extern TASK_LIST_PTR rq_task_tail;

/*-----------------debug procedures ----------------*/
extern void display_ready_list(void);
extern void display_task_list(void);
extern void display_exchange_list(void);
extern void display_delay_list(void);
/*---------------- utility procedures --------------*/

extern unsigned int Ts2Ms (struct timespec *ts);
extern struct timespec Ms2Ts (unsigned int* t);
extern struct  timespec tsAdd (struct timespec  time1,struct timespec time2);
extern struct  timespec tsSubtract (struct timespec time1,struct timespec time2);

extern FILE *logfile;
extern int printlog (const char *fmt, ...);
#define printinfo printlog
extern void printdate (void);
