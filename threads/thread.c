#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
/* 노트. Advanced Scheduling에 따른 헤더 포함 */
#include "threads/fixed_point.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210


/* 노트. 프로젝트 1을 위해 추가된 구조체 */
static struct list sleep_list; // 잠자고 있는 애들에 대한 정보 저장

/* 노트. Advanced Scheduling에 따른 추가된 구조체 */
static struct list all_list;

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/* 노트. Advanced Scheduling에 따른 글로벌 변수 선언 */
int load_avg;

/* Project 3. AP : filesys를 스레드별로 사용할 수 있게 선언 */
extern struct lock filesys_lock;

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) {

	/* 
	 * 노트. Assert 함수는 값이 False이면 시스템을 종료함 (= kernel panic 상태)
	 * inter_get_level 함수는 iterrupt가 꺼져 있는 지 확인하는 함수 (현재는 부팅단계라 꺼져 있어야 함)
	 */
	ASSERT (intr_get_level () == INTR_OFF); 

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* Init the globlal thread context */
	lock_init (&tid_lock);					// 노트. tid_lock 초기화
	list_init (&destruction_req);

	list_init (&sleep_list); 				// 노트. sleep_list를 사용하기 위한 초기화 코드 추가 (프로젝트1에 따른 추가 코드)
	list_init (&ready_list);				// 노트. 레디 큐 초기화
	list_init (&all_list);					// 노트. Advanced Scheduling에 따른 코드 추가

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread (); 					// 노트. PCB 초기화 값의 시작 주소를 리턴 (= tid 값)
	init_thread (initial_thread, "main", PRI_DEFAULT); 		// 노트. 우선순위 부여 (0~63, default: 31)
	initial_thread->status = THREAD_RUNNING;				// 노트. PCB 상태를 의미하며 running 상태로 전환
	initial_thread->tid = allocate_tid ();					// 노트. tid값 + 1

	/*
	 * 노트. PCB(프로세스 제어 블록, Process Control Block)는 특정한 프로세스를 관리할 필요가 있는 정보를 포함하는 운영 체제 커널의 자료 구조 (= TCB)
	 */
}

/* 노트. 스레드 시작 함수 */
/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) {

	/*
	 * 노트.
	 * thread_create() : 새 스레드 생성시 꼭 사용
	 * PRI_MIN : 우선순위 가장 낮음 (= 0)
	 * idle : 지금 만드는 새 스레드가 수행할 함수
	 * &idle_started : 위 함수에 대한 파라미터
	 */
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* 노트. Advanced Scheduling에 따른 추가 */
	load_avg = LOAD_AVG_DEFAULT;

	/* 노트. 인터럽트 시작함수로 케이스 1의 경우 타이머가 시작됨 */
	/* Start preemptive thread scheduling. */
	intr_enable ();

	/* 노트. 쓰레드 교환(실행 -> 대기) */
	/* Wait for the idle thread to initialize idle_thread. */
	sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/*
 * 노트. 스레드 생성 함수가 하는 일
 *	- 스레드 할당과 초기화
 * 	- switch_threads()와 kernel_thread()를 위한 가짜 스택 프레임 생성
 */

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL); // 노트. function을 전달 받지 못하면 ASSERT

	/* 노트. 첫 번째 하는 일 - 스레드 할당하는 부분 */
	/* Allocate thread. */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* 노트. PCB의 스택에 새로운 스레드의 초기값 삽입 - 스택 초기화 */
	/* Initialize thread. */
	init_thread (t, name, priority);
	
	/* Proj 2-4. file descriptor */
	//t->fdTable = palloc_get_page(PAL_ZERO); 				// 단일 페이지 할당 받고 0으로 초기화
	t->fdTable = palloc_get_multiple(PAL_ZERO, FDT_PAGES);	// multi-oom : need more pages to accomodate 10 stacks of 126 opens
	if (t->fdTable == NULL)
		return TID_ERROR;
	t->fdIdx = 2; 											// 0은 stdin, 1은 stdout이기 때문
	
	t->fdTable[0] = 1; 										// fd가 0일 때의 값을 구분하기 위함 (나머지는 NULL)
	t->fdTable[1] = 2;	 									// fd가 1일 때의 값을 구분하기 위함 (나머지는 NULL)

	/* Proj 2-7. Extra */
	/* stdin, stdout count 초기값 설정 */
	t->stdin_count = 1;
	t->stdout_count = 1;

	tid = t->tid = allocate_tid ();

	/* Proj 2-3. fork syscall */
	struct thread *cur = thread_current();
	list_push_back(&cur->child_list, &t->child_elem); // 부모의 child_list에 자식의 child_elem을 넣음

	/* 노트. 두 번째 하는 일 - 커널 스레드를 위한 스택 프레임 (스레드 교환 할 때 필요) */
	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* 노트. 스레드 최초 실행 시에는 프로세스 대기 상태, 즉 레디 큐에 넣어야 함 */
	/* Add to run queue. */
	thread_unblock (t);
	thread_test_preemption ();

	return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);
	
	/* 노트. Priority Scheduling에 따른 push 함수 변경 */
	// 최초 버전
	// list_push_back (&ready_list, &t->elem); // 노트. ready_list에 스레드 추가
	// 추가 버전
	list_insert_ordered (&ready_list, &t->elem, thread_compare_priority, 0);
	
	t->status = THREAD_READY;
	intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/* 노트. Advanced Scheduling에 따른 all_list에서 제거 */
	list_remove(&thread_current()->allelem);

	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	if (curr != idle_thread)
	{
		/* 노트. Priority Scheduling에 따른 push 함수 변경 */
		// 최초 버전
		// list_push_back (&ready_list, &curr->elem); // 노트. ready_list에 스레드 추가
		// 추가 버전
		list_insert_ordered (&ready_list, &curr->elem, thread_compare_priority, 0);
	}
		
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

/* 노트. 현재 진행 중인 스레드의 priority가 변경 되는 경우, donations 리스트에 있는 애들 보다 높아질 수 있음 
 * 이때는 새로 바뀐 priority가 적용될 수 있도록 조치해야 함
 */
/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) {

	/* Advanced Scheduling에 따라 thread_mlfqs 조건 추가 */
	/* priority donation 을 mlfqs 에서는 비활성화 */
	if (thread_mlfqs)
	{
		return;
	}

	thread_current ()->init_priority = new_priority;

	refresh_priority ();
	thread_test_preemption ();
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
/* 노트. Advanced Scheduling에 따라 함수 구현 */
void
thread_set_nice (int nice UNUSED) // 현재 스레드의 nice 값을 새 값으로 설정
{ 	
	// 노트. git 보고 추가한 부분
	ASSERT (nice >= NICE_MIN);
	ASSERT (nice <= NICE_MAX);
	ASSERT (thread_current () != idle_thread);

	enum intr_level old_level = intr_disable ();
	thread_current ()->nice = nice;
	mlfqs_calculate_priority (thread_current ());
	thread_test_preemption ();
	intr_set_level (old_level);
}

/* Returns the current thread's nice value. */
/* 노트. Advanced Scheduling에 따라 함수 구현 */
int
thread_get_nice (void) // 현재 스레드의 nice 값을 반환
{ 	
	enum intr_level old_level = intr_disable ();
	int nice = thread_current ()-> nice;
	intr_set_level (old_level);
	return nice;
}

/* Returns 100 times the system load average. */
/* 노트. Advanced Scheduling에 따라 함수 구현
 * pintos document 의 지시대로 100 을 곱한 후 정수형으로 만들고 반올림하여 반환
 * 정수형 반환값에서 소수점 2째 자리까지의 값을 확인할 수 있도록 하는 용도
 */
int
thread_get_load_avg (void) // 현재 시스템의 load_avg * 100 값을 반환
{ 
	enum intr_level old_level = intr_disable ();
	int load_avg_value = fp_to_int_round (mult_mixed (load_avg, 100));
	intr_set_level (old_level);
	return load_avg_value;
}

/* Returns 100 times the current thread's recent_cpu value. */
/* 노트. Advanced Scheduling에 따라 함수 구현
 * pintos document 의 지시대로 100 을 곱한 후 정수형으로 만들고 반올림하여 반환
 * 정수형 반환값에서 소수점 2째 자리까지의 값을 확인할 수 있도록 하는 용도
 */
int
thread_get_recent_cpu (void) { // 현재 스레드의 recent_cpu * 100 값을 반환
	enum intr_level old_level = intr_disable ();
	int recent_cpu= fp_to_int_round (mult_mixed (thread_current ()->recent_cpu, 100));
	intr_set_level (old_level);
	return recent_cpu;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* Let someone else run. */
		intr_disable ();
		thread_block ();

		/* Re-enable interrupts and wait for the next one.

		   The `sti' instruction disables interrupts until the
		   completion of the next instruction, so these two
		   instructions are executed atomically.  This atomicity is
		   important; otherwise, an interrupt could be handled
		   between re-enabling interrupts and waiting for the next
		   one to occur, wasting as much as one clock tick worth of
		   time.

		   See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
		   7.11.1 "HLT Instruction". */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* The scheduler runs with interrupts off. */
	function (aux);       /* Execute the thread function. */
	thread_exit ();       /* If function() returns, kill the thread. */
}


/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->magic = THREAD_MAGIC;

	/* 노트. Priority Scheduling에 따른 초기화 코드 추가 */
	t->init_priority = priority;
	t->wait_on_lock = NULL;
	list_init (&t->donations);

	/* 노트. Advanced Scheduling에 따른 초기화 코드 추가
	 * 변수 추가에 따른 초기화
	 */ 
	t->nice = NICE_DEFAULT;
	t->recent_cpu = RECENT_CPU_DEFAULT;
	list_push_back(&all_list, &t->allelem);

	/* Proj 2-3. fork syscall */
	list_init(&t->child_list);
	sema_init(&t->fork_sema, 0);

	/* Proj 2-3. wait syscall */
	sema_init(&t->wait_sema, 0);
	sema_init(&t->free_sema, 0);

	/* Proj 2-6. Denying write to executable */
	t->running = NULL;	// 구조체 추가에 따른 초기화
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile (
			/* Store registers that will be used. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* Fetch input once */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // read the current rip.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current()->status == THREAD_RUNNING);
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current ()->status = status;
	schedule ();
}

static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate (next);
#endif

	if (curr != next) {
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used bye the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch (next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}

// Note. 스레드를 ticks 시각까지 재우는 함수
void
thread_sleep (int64_t ticks)
{
	struct thread *cur;

	// Note. 인터럽트를 금지하고, 이전의 인터럽트 레벨을 저장
	enum intr_level old_level;
	old_level = intr_disable();

	// Note. 현재 스레드 정보를 저장하고, cur이 idle 스레드이면 슬립되지 않게 조치
	cur = thread_current();
	ASSERT(cur != idle_thread);

	// Note. 스레드가 일어나야 하는 tick 값을 업데이트 (이때 awake 함수가 깨워줌)
	cur->wakeup = ticks;

	// Note. 현재 스레드를 슬립 큐에 삽입한 후 스케줄
	list_push_back(&sleep_list, &cur->elem);

	// Note. 큐에 삽인한 후 스레드를 블락하고 다음 스케줄 있을 때까지 블락된 상태로 대기
	thread_block();

	// Note. 인터럽트를 다시 받아들일 수 있도록 수정
	intr_set_level(old_level);
}

// Note. ticks가 되면 자고 있는 스레드를 깨우는 함수
void thread_awake(int64_t ticks)
{
	struct list_elem *e = list_begin (&sleep_list);

	while (e != list_end(&sleep_list))
	{
		struct thread *t = list_entry(e, struct thread, elem);

		if (t->wakeup <= ticks)
		{
			e = list_remove(e);
			thread_unblock(t);
		}
		else
		{
			e = list_next(e);
		}
	}
}

// 노트. Priority Scheduling에 따른 추가 함수
bool
thread_compare_priority(struct list_elem *add_elem, struct list_elem *position_elem, void *aux UNUSED)
{
	return list_entry (add_elem, struct thread, elem)->priority
			> list_entry (position_elem, struct thread, elem)->priority;
}

// 노트. Priority Scheduling에 따른 추가 함수
void
thread_test_preemption (void)
{
	if(list_empty(&ready_list))
	{
		return;
	}

	struct thread *t = list_entry (list_front (&ready_list), struct thread, elem);

	if(intr_context())
	{
		thread_ticks++;
		if (thread_ticks >= TIME_SLICE && thread_current()->priority == t->priority)
		{
      		intr_yield_on_return();
      		return;
    	}
	}
	else {
		if(thread_current()->priority < t->priority)
		{
			thread_yield();
		}
	}

	// if (!intr_context() && !list_empty (&ready_list) &&
	// thread_current ()->priority <
	// list_entry (list_front (&ready_list), struct thread, elem)->priority)
	// {
	// 	thread_yield();
	// }
}

/* 노트. Priority Scheduling에 따른 함수 추가 */
bool
thread_compare_donate_priority (const struct list_elem *add_elem, 
				const struct list_elem *position_elem, void *aux UNUSED)
{
	return list_entry (add_elem, struct thread, donation_elem)->priority
		 > list_entry (position_elem, struct thread, donation_elem)->priority;
}

/* 노트. Priority Scheduling에 따른 함수 추가 */
void
donate_priority (void)
{
  int depth; // nested의 최대 깊이 지정 (max_depth = 8)
  struct thread *cur = thread_current ();

  for (depth = 0; depth < 8; depth++)
  {
	// wait_on_lock이 null이 아니라면 스레드에 lock이 걸려 있다는 뜻으로 holder 스레드에게 priority 양도
    if (!cur->wait_on_lock) break;
      struct thread *holder = cur->wait_on_lock->holder;
      holder->priority = cur->priority;
      cur = holder;
  }
}

/* 노트. Advanced Scheduling에 따른 함수 추가
 * 특정 스레드의 priority 를 계산하는 함수
 * idle_thread의 priority는 고정이므로 제외하고 fp 연산 함수를 사용
 * 계산 결과의 소수분은 버림하고 정수의 priority로 설정
 */
void
mlfqs_calculate_priority (struct thread *t)
{
  if (t == idle_thread)
  {
	  return ;
  }
  t->priority = fp_to_int (add_mixed (div_mixed (t->recent_cpu, -4), PRI_MAX - t->nice * 2));
}

/* 노트. Advanced Scheduling에 따른 함수 추가
 * recent_cpu 값을 계산하는 함수
 */
void
mlfqs_calculate_recent_cpu (struct thread *t)
{
  if (t == idle_thread)
  {
	  return ;
  }
  t->recent_cpu = add_mixed (mult_fp (div_fp (mult_mixed (load_avg, 2), add_mixed (mult_mixed (load_avg, 2), 1)), t->recent_cpu), t->nice);
}

/* 노트. Advanced Scheduling에 따른 함수 추가
 * load_avg 값을 계산하는 함수
 * load_avg 값을 스레드 고유의 값이 아니라 system wide 값이기 때문에 idle_thread 가 실행되는 경우에도 계산
 * ready_threads 는 현재 시점에서 실행 가능한 스레드의 수를 나타내므로 ready_list 에 들어있는 스레드의 숫자에 현재 running 스레드 1개를 더함 (idle_thread는 제외)
 */
void 
mlfqs_calculate_load_avg (void) 
{
	int ready_threads;
	
	if (thread_current () == idle_thread)
		ready_threads = list_size (&ready_list);
	else
		ready_threads = list_size (&ready_list) + 1;

	load_avg = add_fp (mult_fp (div_fp (int_to_fp (59), int_to_fp (60)), load_avg), 
						mult_mixed (div_fp (int_to_fp (1), int_to_fp (60)), ready_threads));

	// 노트. git 보고 추가한 부분
	ASSERT(load_avg>=0);
}

/* 노트. Advanced Scheduling에 따른 함수 추가
 * 각각의 값들이 변하는 시점에 수행 함수를 생성
 * 변하는 시점은 총 3가지 경우
 * - 1 tick 마다 running 스레드의 recent_cpu 값 + 1
 * - 4 tick 마다 모든 스레드의 priority 재계산
 * - 1 초마다 모든 스레드의 recent_cpu 값과 load_avg 값 재계산
 */
void
mlfqs_increment_recent_cpu (void) // 현재 스레드의 recent_cpu의 값을 1 증가 시키는 함수
{
  if (thread_current () != idle_thread)
    thread_current ()->recent_cpu = add_mixed (thread_current ()->recent_cpu, 1);
}

void
mlfqs_recalculate_recent_cpu (void) // 모든 스레드의 recent_cpu를 재계산 하는 함수
{
  struct list_elem *e;

  for (e = list_begin (&all_list); e != list_end (&all_list); e = list_next (e)) {
    struct thread *t = list_entry (e, struct thread, allelem);
    mlfqs_calculate_recent_cpu (t);
  }
}

void
mlfqs_recalculate_priority (void) // 모든 스레드의 priority를 재계산하는 함수
{
  struct list_elem *e;

  for (e = list_begin (&all_list); e != list_end (&all_list); e = list_next (e)) {
    struct thread *t = list_entry (e, struct thread, allelem);
    mlfqs_calculate_priority (t);
  }
}