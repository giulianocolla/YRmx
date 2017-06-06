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

/* Some useful constants */

#define	true		0xff
#define false		0
#define forever		while (true)

/* system constants */
#define TIMER_8254_IRQ	0
#define APIC_TIMER_IRQ IPIPE_SERVICE_IPI3

//#define YALRT_USES_APIC_TIMER

#ifdef YALRT_USES_APIC_TIMER
//#define TIMER_IRQ APIC_TIMER_IRQ
#define TIMER_IRQ LOCAL_TIMER_VECTOR
#else
#define TIMER_IRQ TIMER_8254_IRQ
#endif

/* Structures to hold data which must be saved/restored when switching context from yalrt to Linux and back
  Lifted from RTAI */
typedef struct {
		volatile unsigned long sflags;
		volatile unsigned long lflags;
#if defined(CONFIG_X86_LOCAL_APIC) && defined(RTAI_TASKPRI)
		volatile unsigned long set_taskpri;
#endif
  } CONTEXT_SWITCH_DATA;

/* Further declarations for context switch - Lifted from RTAI */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,17) || (defined(CONFIG_PPC) && LINUX_VERSION_CODE > KERNEL_VERSION(2,6,13))
#define hal_current_domain(cpuid)  per_cpu(ipipe_percpu_domain, cpuid) 
#else
#define hal_current_domain(cpuid)  (ipipe_percpu_domain[cpuid])
#endif

#if defined(CONFIG_X86_LOCAL_APIC) && defined(RTAI_TASKPRI)
#define SET_TASKPRI(cpuid) \
		if (!linux_context_data[cpuid].set_taskpri) { \
				apic_write_around(APIC_TASKPRI, ((apic_read(APIC_TASKPRI) & ~APIC_TPRI_MASK) | RTAI_TASKPRI)); \
				linux_context_data[cpuid].set_taskpri = 1; \
		}
#define CLR_TASKPRI(cpuid) \
		if (linux_context_data[cpuid].set_taskpri) { \
				apic_write_around(APIC_TASKPRI, (apic_read(APIC_TASKPRI) & ~APIC_TPRI_MASK)); \
				linux_context_data[cpuid].set_taskpri = 0; \
		}
#else
#define SET_TASKPRI(cpuid)
#define CLR_TASKPRI(cpuid)
#endif

#define ROOT_STATUS_ADR(cpuid)  (&ipipe_cpudom_var(ipipe_root_domain, status))
#define ROOT_STATUS_VAL(cpuid)  (ipipe_cpudom_var(ipipe_root_domain, status))

#ifdef RTAI_TRIOSS

#define _rt_switch_to_real_time(cpuid) \
do { \
		linux_context_data[cpuid].lflags = xchg(&DOMAIN_TO_STALL->cpudata[cpuid].status, (1 << IPIPE_STALL_FLAG)); \
		linux_context_data[cpuid].oldomain = hal_current_domain(cpuid); \
		linux_context_data[cpuid].sflags = 1; \
		hal_current_domain(cpuid) = &yalrt_domain; \
} while (0)

#define rt_switch_to_linux(cpuid) \
do { \
		if (linux_context_data[cpuid].sflags) { \
				hal_current_domain(cpuid) = (void *)linux_context_data[cpuid].oldomain; \
				DOMAIN_TO_STALL->cpudata[cpuid].status = linux_context_data[cpuid].lflags; \
				linux_context_data[cpuid].sflags = 0; \
				CLR_TASKPRI(cpuid); \
		} \
} while (0)

#else

#define _rt_switch_to_real_time(cpuid) \
do { \
		linux_context_data[cpuid].lflags = xchg(ROOT_STATUS_ADR(cpuid), (1 << IPIPE_STALL_FLAG)); \
		linux_context_data[cpuid].sflags = 1; \
		hal_current_domain(cpuid) = &yalrt_domain; \
} while (0)

#define rt_switch_to_linux(cpuid) \
do { \
		if (linux_context_data[cpuid].sflags) { \
				hal_current_domain(cpuid) = ipipe_root_domain; \
				ROOT_STATUS_VAL(cpuid) = linux_context_data[cpuid].lflags; \
				linux_context_data[cpuid].sflags = 0; \
				CLR_TASKPRI(cpuid); \
		} \
} while (0)

#endif

#define rt_switch_to_real_time(cpuid) \
do { \
		if (!linux_context_data[cpuid].sflags) { \
				_rt_switch_to_real_time(cpuid); \
		} \
} while (0)

#define rt_restore_switch_to_linux(sflags, cpuid) \
do { \
	if (!sflags) { \
		rt_switch_to_linux(cpuid); \
	} else if (!linux_context_data[cpuid].sflags) { \
		SET_TASKPRI(cpuid); \
		_rt_switch_to_real_time(cpuid); \
	} \
} while (0)
																	
#define LOCK_LINUX(cpuid) \
	do { rt_switch_to_real_time(cpuid); } while (0)
#define UNLOCK_LINUX(cpuid) \
	do { rt_switch_to_linux(cpuid);	 } while (0)

#define SAVE_LOCK_LINUX(cpuid) \
	do { sflags = rt_save_switch_to_real_time(cpuid); } while (0)
#define RESTORE_UNLOCK_LINUX(cpuid) \
	do { rt_restore_switch_to_linux(sflags, cpuid);   } while (0)
			
/* to get rid of some weird crashes when using RTAI FIFO's - arcane */
//#define USE_FIFO

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

/* uncomment the line below if you want to experiment with rqssnd instead of rqisnd
for srq service, such as fifo handlers. no great advantage in doing so */

#define SSND_SUPPORT

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
*/

typedef struct exchange_descriptor  {
		struct msg_descriptor *message_head;
		struct msg_descriptor *message_tail;
		struct task_descriptor *task_head;
		struct task_descriptor *task_tail;
		struct exchange_descriptor *exchange_link;
		unsigned char	name[6];
		} EXCHANGE_DESCRIPTOR;
		
#define exchange_descriptor_length (sizeof (EXCHANGE_DESCRIPTOR))
#define EXCH_NAME_LEN 6

/*
* An Interrupt Exchange is nothing but a regular exchange, with an associated
* message, which may be linked to the exchange like any other message.
* The Interrupt message has special fields for interrupt handling.
* This setup makes interrupt handling as fast as possible
*/

typedef struct	int_exchange_descriptor {
		struct msg_descriptor *message_head;
		struct msg_descriptor *message_tail;
		struct task_descriptor *task_head;
		struct task_descriptor *task_tail;
		struct exchange_descriptor *exchangelink;
		unsigned char	name[6];
		unsigned char assigned;
		int irq;
		void *yalrt_handler;
		struct msg_descriptor *link;
		unsigned int length;
		unsigned int type;
#ifndef USE_FIFO
		unsigned int yltime;
#endif
		unsigned char level;
		unsigned char qint;
		unsigned char fill;
		} INT_EXCHANGE_DESCRIPTOR;

#define int_exchange_length (sizeof(INT_EXCHANGE_DESCRIPTOR))

typedef struct int_msg_descriptor {
		struct msg_descriptor *link;
		unsigned int length;
		unsigned int type;
#ifndef USE_FIFO
		unsigned int yltime;
#endif
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

#ifndef USE_FIFO
#define msghdr \
		struct msg_descriptor *link; \
		unsigned int length; \
		unsigned int type;\
		unsigned int yltime; \
		struct exchange_descriptor *home_exchange; \
		struct exchange_descriptor *response_exchange
#else
#define msghdr \
		struct msg_descriptor *link; \
		unsigned int length; \
		unsigned int type;\
		struct exchange_descriptor *home_exchange; \
		struct exchange_descriptor *response_exchange
#endif

typedef struct	msg_descriptor {
		msghdr;
		unsigned char user_defined [1];
		} __attribute__ ((packed)) MSG_DESCRIPTOR;

#ifdef RMX_COMPATIBLE
#pragma pack(1)
#endif
//typedef struct	msg_descriptor {
//		msghdr;
//		unsigned char user_defined [1];
//		} MSG_DESCRIPTOR;

// for the timeout message
struct sys_msg {
	struct msg_descriptor *link;
	unsigned int length;
	unsigned int type;
#ifndef USE_FIFO
	unsigned int yltime;
#endif
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

/* Possible content of the taskndp field */	
#define ndpused  1


#define task_links \
	struct task_descriptor *next;	/*linkforward	 */\
	struct task_descriptor *prev;	/*linkback		 */\
	struct task_descriptor *thread; /*thread */\
	unsigned short delay;

#define td_middle_part\
	struct exchange_descriptor *exchange; \
	void *			sp;\
	void *			marker;\
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

#define task_descriptor_length  sizeof(TASK_DESCRIPTOR)
#define NDP_SIZE 94  /* 8087 save area - extend to 108 for 80386 */

typedef struct ndp_task_descriptor {
		task_links;
		struct msg_descriptor *message;
		td_middle_part;
		td_end_part;
		unsigned char ndpsavearea[NDP_SIZE];
		} ndp_task_descriptor_t;

#define ndp_task_descriptor_length  sizeof(ndp_task_descriptor_t)
// ndp not (yet) supported
#define reqndpwait()
#define ndpinit()

#define	delayed		0x01
#define	suspended	0x02
#define	maskint		0x04
#define	ndptask		0x08
#define running		0x10
#define deleted		0x40
#define waiting		0x80

#define bottomflag		0xc7c7
#define unusedflag		0xc7

typedef TASK_DESCRIPTOR *TASK_LIST_PTR;
typedef EXCHANGE_DESCRIPTOR *EXCHANGE_LIST_PTR;
typedef INT_EXCHANGE_DESCRIPTOR *INT_EXCHANGE_LIST_PTR;

#ifdef YARTL_STANDALONE
#define hard_sti() __asm__ __volatile__ ("sti": : :"memory")
#define hard_cli() __asm__ __volatile__ ("cli": : :"memory")
#define hard_save_flags(x) \
__asm__ __volatile__("pushfl ; popl %0":"=g" (x): /* no input */ :"memory")
#define hard_restore_flags(x) \
__asm__ __volatile__("pushl %0 ; popfl": /* no output */ :"g" (x):"memory")
#endif

#define get_stack_pointer(sp) \
	__asm__ __volatile__( \
	"movl %%esp, (%0)\n\t" \
	: \
	: "c" (sp) );

#define set_stack_pointer(sp) \
	__asm__ __volatile__( \
	"movl (%0), %%esp\n\t" \
	: \
	: "c" (sp) : "memory" );

#define save_ds() __asm__ __volatile__ ("pushl %ds")
#define restore_ds() __asm__ __volatile__ ("popl %ds")

extern int rqsystick;
extern unsigned int rqsystime;
extern TASK_LIST_PTR rqactv;

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

extern void reqisnd (INT_EXCHANGE_DESCRIPTOR *);
#ifdef SSND_SUPPORT
extern void reqssnd (INT_EXCHANGE_DESCRIPTOR *);
#endif
extern void reqendi (INT_EXCHANGE_DESCRIPTOR *);

#define NR_GLOBAL_IRQ	32
extern INT_EXCHANGE_DESCRIPTOR rqlintex[NR_GLOBAL_IRQ] ;

extern void exit_region(int FromInt);
extern int critical_region_flag;
#define enter_region ++critical_region_flag

/* skipbyte function not available in include */

#if 0 /*FIXED_486_STRING && (CPU == 486 || CPU == 586) */
inline void * memrchr(const void * cs,int c,size_t count)
{
register void * __res;
if (!count)
	return NULL;
__asm__ __volatile__(
	"cld\n\t"
	"repe\n\t"
	"scasb\n\t"
	"jne 1f\n\t"
	"movl $1,%0\n"
	"1:\tdecl %0"
	:"=D" (__res):"a" (c),"D" (cs),"c" (count)
	:"cx");
return __res;
}
#else
inline void * memrchr(const void * cs,int c,size_t count)
{
int d0;
register void * __res;
if (!count)
	return NULL;
__asm__ __volatile__(
	"cld\n\t"
	"repe\n\t"
	"scasb\n\t"
	"jne 1f\n\t"
	"movl $1,%0\n"
	"1:\tdecl %0"
	:"=D" (__res), "=&c" (d0) : "a" (c),"0" (cs),"1" (count));
return __res;
}
#endif

/* user interface calls - to have same calls from user space */
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

#ifdef CONFIG_PROC_FS
#define LIMIT (PAGE_SIZE-80)
extern struct proc_dir_entry *yalrt_proc_root;
extern EXCHANGE_LIST_PTR rq_exchange_head;
extern EXCHANGE_LIST_PTR rq_exchange_tail;
extern TASK_LIST_PTR rq_task_head;
extern TASK_LIST_PTR rq_task_tail;
#endif
