#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"

/* Proj 2-2. synch header added */
#include "threads/synch.h"

#ifdef VM
#include "vm/vm.h"
#endif


/* States in a thread's life cycle. */
enum thread_status {
	THREAD_RUNNING,     /* Running thread. */
	THREAD_READY,       /* Not running but ready to run. */
	THREAD_BLOCKED,     /* Waiting for an event to trigger. */
	THREAD_DYING        /* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

/* 노트. Advanced Scheduling에 따른 추가된 정의 */            
#define RECENT_CPU_DEFAULT 0
#define LOAD_AVG_DEFAULT 0
#define NICE_MIN -20
#define NICE_DEFAULT 0
#define NICE_MAX 20



/* A kernel thread or user process.
 *
 * Each thread structure is stored in its own 4 kB page.  The
 * thread structure itself sits at the very bottom of the page
 * (at offset 0).  The rest of the page is reserved for the
 * thread's kernel stack, which grows downward from the top of
 * the page (at offset 4 kB).  Here's an illustration:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * The upshot of this is twofold:
 *
 *    1. First, `struct thread' must not be allowed to grow too
 *       big.  If it does, then there will not be enough room for
 *       the kernel stack.  Our base `struct thread' is only a
 *       few bytes in size.  It probably should stay well under 1
 *       kB.
 *
 *    2. Second, kernel stacks must not be allowed to grow too
 *       large.  If a stack overflows, it will corrupt the thread
 *       state.  Thus, kernel functions should not allocate large
 *       structures or arrays as non-static local variables.  Use
 *       dynamic allocation with malloc() or palloc_get_page()
 *       instead.
 *
 * The first symptom of either of these problems will probably be
 * an assertion failure in thread_current(), which checks that
 * the `magic' member of the running thread's `struct thread' is
 * set to THREAD_MAGIC.  Stack overflow will normally change this
 * value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
 * the run queue (thread.c), or it can be an element in a
 * semaphore wait list (synch.c).  It can be used these two ways
 * only because they are mutually exclusive: only a thread in the
 * ready state is on the run queue, whereas only a thread in the
 * blocked state is on a semaphore wait list. */

/*
 * 노트. thread 하나 생성시 구조체 4kb가 하나씩 생성
 * 구조체에서 tid부터 magic까지의 구간이 스레드의 PCB에 해당 (0kb ~ magic)
 * 4kb에서 PCB를 뺀 나머지는 kernel stack(해당 스레드 실행하며 사용할 스택 공간)임
 * 스택 공간에 데이터가 쌓이다가 size가 초과되면 magic으로 넘어가서 오버플로우 감시 (magic의 역할)
 * thread 구조체 = PCB + Kernel Stack
 */
struct thread {

	/* Owned by thread.c. */
	tid_t tid;                          /* Thread identifier. 고유번호 */
	enum thread_status status;          /* Thread state. 스레드 상태 */
	char name[16];                      /* Name (for debugging purposes). 스레드 명 */
	int priority;                       /* Priority. 우선순위 저장 */

	int64_t wakeup;						// 노트. thread마다 깨어나야 할 tick에 대한 저장 변수 필요 및 추가 (프로젝트1에 따른 추가 코드)
    int init_priority;         			// 노트. 스레드가 priority 를 양도받았다가 다시 반납할 때 원래의 priority 를 복원할 수 있도록 고유의 priority 값을 저장하는 변수
    
    struct lock *wait_on_lock;  		// 노트. 스레드가 현재 얻기 위해 기다리고 있는 lock 으로 스레드는 이 lock 이 release 되기를 기다림
    struct list donations;      		// 노트. 자신에게 priority 를 나누어준 스레드들의 리스트
    struct list_elem donation_elem; 	// 노트. donations 리스트를 관리하기 위한 element 로 thread 구조체의 그냥 elem 과 구분하여 사용

	/* 노트. Advanced Scheduling에 따른 변수 추가
	 * nice, recent_cpu 담을 변수 추가
	 */
	int nice;
	int recent_cpu;

	/* Proj 2-3. fork syscall */
	struct intr_frame parent_if; 		// 노트. fork 시, child 프로세스에게 현재 나의 intr_frame 정보를 전달하기 위함 (child 입장에서는 parent_if)
	struct list child_list; 			// 노트. 자신에게 fork 된 child 프로세스들의 리스트
	struct list_elem child_elem; 			// 노트. child 프로세스 리스트를 관리하기 위해 별도로 구분한 element
	struct semaphore fork_sema; 		// 노트. child fork가 __do_fork, 즉 fork를 완료할 때까지 기다리기 위한 sema

	/* Proj 2-3. wait syscall */
	struct semaphore wait_sema;			// 노트. child 프로세스를 기다리기 위해 사용
	int exit_status;					// 노트. child 프로세스의 exit 상태를 parent에 전달하기 위함
	struct semaphore free_sema; 		// 노트. parent가 wait 함수에서 exit_status 값을 받을 때까지 child 프로세스 종료 연기

	/* Shared between thread.c and synch.c. */
	struct list_elem elem;              /* List element. */

#ifdef USERPROG
	/* Owned by userprog/process.c. */
	uint64_t *pml4;                     /* Page map level 4 */
#endif
#ifdef VM
	/* Table for whole virtual memory owned by thread. */
	struct supplemental_page_table spt;
#endif

	/* Owned by thread.c. */
	struct intr_frame tf;               /* Information for switching */
	unsigned magic;                     /* Detects stack overflow. */

	/* 노트. Advanced Scheduling에 따른 추가된 구조체 */
	struct list_elem allelem;			/* all_list element. */
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void do_iret (struct intr_frame *tf);


/* Note. 프로젝트 1을 위해 추가된 함수 선언 */
void thread_sleep(int64_t ticks); // Note. 스레드를 ticks 시각까지 재우는 함수
void thread_awake(int64_t ticks); // Note. 일어나야 할 ticks 시각이 되면 스레드를 깨우는 함수

/* 노트. Priority Scheduling을 위해 추가된 함수 */
bool thread_compare_priority(struct list_elem *add_elem, struct list_elem *position_elem, void *aux UNUSED);
void thread_test_preemption (void);
void donate_priority (void);

/* 노트. Advanced Scheduling을 위해 추가된 함수 */
void mlfqs_calculate_priority (struct thread *t);	// 특정 thread의 prioirity 계산
void mlfqs_calculate_recent_cpu (struct thread *t);	//스레드의 recent_cpu 계산하는 함수
void mlfqs_calculate_load_avg (void); // load_avg 값을 계산

void mlfqs_increment_recent_cpu (void);	// 현재 스레드의 recent_cpu 값을 1 증가시킴
void mlfds_recalculate_recent_cpu (void);	// 모든 스레드의 recent_cpu 를 재계산 하는 함수
void mlfqs_recalculate_priority (void);	//모든 스레드의 priority 재계산

#endif /* threads/thread.h */
