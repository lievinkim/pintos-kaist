#include "threads/init.h"
#include <console.h>
#include <debug.h>
#include <limits.h>
#include <random.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "devices/kbd.h"
#include "devices/input.h"
#include "devices/serial.h"
#include "devices/timer.h"
#include "devices/vga.h"
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/loader.h"
#include "threads/malloc.h"
#include "threads/mmu.h"
#include "threads/palloc.h"
#include "threads/pte.h"
#include "threads/thread.h"

/* 노트. USERPROG가 정의되어 있는 경우에만 실행 */
#ifdef USERPROG
#include "userprog/process.h"
#include "userprog/exception.h"
#include "userprog/gdt.h"
#include "userprog/syscall.h"
#include "userprog/tss.h"
#endif

#include "tests/threads/tests.h"

/* 노트. VM이 정의되어 있는 경우에만 실행 */
#ifdef VM
#include "vm/vm.h"
#endif

/* 노트. FILESYS가 정의되어 있는 경우에만 실행 */
#ifdef FILESYS
#include "devices/disk.h"
#include "filesys/filesys.h"
#include "filesys/fsutil.h"
#endif

/* 노트. Page-map-level-4는 페이지 테이블 라벨링 결정하는 방식이다 */
/* Page-map-level-4 with kernel mappings only. */
uint64_t *base_pml4;

/* 노트. FILESYS가 정의되어 있는 경우에만 실행 */
#ifdef FILESYS
/* -f: Format the file system? */
static bool format_filesys;
#endif

/* 노트. 커널 업무가 완료된 후 전원 종료 */
/* -q: Power off after kernel tasks complete? */
bool power_off_when_done;

bool thread_tests;

static void bss_init (void);
static void paging_init (uint64_t mem_end);

static char **read_command_line (void);
static char **parse_options (char **argv);
static void run_actions (char **argv);
static void usage (void);

static void print_stats (void);


int main (void) NO_RETURN;

/* 노트. 핀토스 메인 프로그램 */
/* Pintos main program. */
int
main (void) {
	uint64_t mem_end;
	char **argv;

	/*
	 * 노트. BSS(Block Started By Symbol) Segment는 초기화 되지 않은 data segment를 의미함
	 * 해당 세그먼트의 데이터는 커널에 의해 프로그램이 실행하기 직전 0으로 초기화 되며,
	 * 초기화되지 않은 데이터는 데이터 세그먼트 끝 시점에 시작하며 소스코드 내 0 또는 특별한 초기화를 하지 않은 모든 글로벌 및 정적 변수에 대한 정보를 갖는다.
	 */
	/* Clear BSS and get machine's RAM size. */
	bss_init ();

	/* Break command line into arguments and parse options. */
	argv = read_command_line (); 	// 노트. pintos -v -- run alarm-multiple에서 run과 alarm-multiple 실행 인자를 읽음
	argv = parse_options (argv); 	// 노트. -v 등과 같은 함수 옵션을 분석

	/* 노트. 스레드 및 콘솔 초기화 */
	/* Initialize ourselves as a thread so we can use locks,
	   then enable console locking. */
	thread_init (); 				// 노트. lock을 활성화 시키고 ready list를 초기화하며, 가장 근처 page를 찾아 해당 주소를 기준으로 스레드 정보 설정 (main 스레드, initial/thread)
	console_init (); 				// 노트. console에 lock을 초기화하며, 콘솔 사용 할 수 있어 printf 함수 사용 가능
	
	/* 노트. 메모리 시스템 초기화 */
	/* Initialize memory system. */
	mem_end = palloc_init (); 		// 노트. page allocate 할 수 있도록 초기화
	malloc_init (); 				// 노트. malloc을 통한 메모리 할당 위해 초기화
	paging_init (mem_end); 			// 노트. loader.S에서 구성했던 page table을 다시 구성
	/* 
	 * 노트. 여기서 page table 이란
	 * 메모리를 page로 관리하게 되면서 생긴 각 page에 대한 index를 갖고 있는 table
	 * 관리해야 할 메모리가 커지면서 page table을 관리하는 또 다른 table인 'page directory' 필요
	 */

/* Segmentation 초기화 */
#ifdef USERPROG
	tss_init (); 					// 노트. task state segment를 설정하는 부분으로 커널이 task를 관리할 때 필요한 정보가 들어 있는 segment이다.
	gdt_init (); 					// 노트. global description table을 초기화 하는 부분. 동일하게 task 관리에 필요한 정보가 들어 있으며 주로 메모리 보호나 segment 관련 내용이 들어 있다.
	/* 
	 * 노트. gdt_int() 함수를 더 자세히 보자면
	 * kernel과 user의 code/data segment를 초기화하여 gdt를 구성
	 * 여기서 segment privilege를 설정할 수 있음
	 * 이는 해당 segment에 존재하는 기계어가 CPU의 중요한, 즉 다른 프로그램에 영향을 미칠 수 있는 코드를 수행할 수 있느냐에 대한 권한이다.
	 */

#endif

	/* 노트. 인터럽트 핸들러 초기화 */
	/* Initialize interrupt handlers. */
	intr_init (); 		// 노트. PIC(Programmable Interrupt Controller)를 초기화. PIC는 interrupt 장치에 연결되어 CPU에게 interrupt 신호를 보내주는 장치.
				  		// Interrupt descriptor table인 idt도 초기화 한다. 이 table은 interrupt를 핸들링 하는 handler 함수들이 연결되는데 여기서는 우선 깨끗히 초기화 한다.
						// 추가로 0번부터 19번까지의 interrupt 이름을 초기화하는데 이는 CPU가 운영체제, 즉 커널에게 전달하는 interrupt이다.
						// PIC에 의해 전달되는 interrupt도 0번부터 시작하지만 0x00부터 0x19까지는 CPU가 커널에게 전달하기 위한 interrupt로 사용하고
						// 그 이후부터가 PIC에 의한 interrupt가 된다. 대표적인 예로 조금 있다 보게 될 keyboard의 interrupt 번호는 0x21이다.

	/* 노트. interrupt table을 초기화 후 각 interrupt에 대해 handler를 연결 */	
	timer_init (); 		// 노트. 과제의 예제로 주어진 thread에서도 사용되는 timer를 먼저 초기화. timer는 PIC 0x00이나 함수 내부적으로 intr_register_ext() 함수를 이용하여 0x20에 timer_interrupt()함수를 연결
	kbd_init (); 		// 노트. 위 방법과 동일한 방법으로 keyboard의 interrupt를 초기화. interrupt는 interrupt queue에 넣어졌다가 interrupt를 처리하는 thread에 의해 처리되는데 해당 동작을 수행하는 모듈은 input이다.
	input_init (); 		// 노트. 초기화
#ifdef USERPROG
	exception_init (); 	// 노트. intr_init()에서 작성한 0x00 ~ 0x13까지의 interrupt를 연결
						// 연결되는 handler를 살펴보면 exception.c의 72번째 줄에 존재하는 kill 함수인데 친절하게도 왜 죽는지에 대해 설명하고 죽는다.
						// 주석을 보면 user program에 의해 process가 잘못된 수행을 했을 경우에 이 handler가 호출된다고 함
	syscall_init (); 	// 노트. system call interrupt를 핸들링 한다. 현재 핸들링하는 system call은 0x30번 interrupt인데 단순히 system call을 출력하고 끝남
#endif
	/* 
	 * 노트. software interrupt(exception), hardware interrupt, system call에 대한 interrupt handler 초기화 및 설정
	 * loader.S에 의해 처음부터 시작하여 중간에 메모리 크기를 측정하고 커널 이미지를 메모리에 올릴 때까지 interrupt를 받지 않으려고 했으나,
	 * 이제는 설정한 interrupt를 동작시킬 수 있도록 threads를 우선 시작한다.
	 */

	/* Start thread scheduler and enable interrupts. */
	thread_start ();			// 노트. 우선 순위가 가장 낮은 idle thread를 생성하여 동작
								// thread_start 함수 내 intr_enable() 함수를 호출하여 interrupt 활성화
								// 활성화 하는 이유는 idle은 생성될 때 interrupt를 disable 하고 본인 자신(idle thread)을 block하여 다른 thread가 먼저 수행할 수 있도록 한다.
								// 그리고 idle은 다시 ready_list에 올라가지 않으며 나중에 next_thread_to_run() 실행 시 thread가 비어 있으면 다시 돌아온다.
	serial_init_queue ();		// 노트. serial로부터 interrupt를 받아 커널을 제어할 수 있도록 함
								// 이는 커널이 올라가 있는 장치에 keyboard로 바로 console을 통해 접속하는 것이 아니라 tty 등의 serial interface로 접근했을 때도 커널이 반응할 수 있도록 하게 함
	timer_calibrate ();			// 노트. 아까 설정한 timer interrupt에 의한 한 tick에 몇 번의 loop을 돌 수 있나 계산해서 전역 변수인 loops_per_tick에 넣어두고 이 값은 여러 sleep() 함수들의 동작을 실제로 수행하는 real_time_sleep() 함수에서 사용
								// 이 값을 사용하여 멈추기를 요구하는 시간을 근사한 loop 회수로 변환하여 busy_wait() 함수에서 지정된 회수만큼 반복문을 돌면서 대기

#ifdef FILESYS
	/* Initialize file system. */
	disk_init ();					// 노트. 커널에서 현재 연결된 storage를 검색해서 초기화
	filesys_init (format_filesys); 	// 노트. disk_init()에서 탐지하고 초기화한 disk를 가져와서 inode를 구성하거나 포맷을 수행
#endif

#ifdef VM
	vm_init ();
#endif

	printf ("Boot complete.\n"); 	// 노트. 부팅이 완료되었음을 출력

	/* Run actions specified on kernel command line. */
	run_actions (argv); 			// 노트. 부트 로더로부터 읽어온 실행인자(명령 이름)를 run_actions 함수에 전달하고, 해당 일이 끝나면 사용자의 요구에 따라 전원을 끄거나 혹은 멈춤
							     	// run_action이 받는 실행인자들과 mapping되는 함수들이 action이라는 구조체에 table로 정의되어 있으며, 실제로 run alarm-multiple의 run도 run task란 함수와 맵핑되어 있음을 확인할 수 있음

	/*
	 * 노트. run_actions 함수를 조금 더 살펴 보자
	 * 
	 * STEP 01.
	 * action은 name, argc, function 3가지를 멤버로 가지고 있는 구조체
	 * ㄴ struct action { char *name; , int argc; , void (*function) (char **argv); };
	 * actions는 여러 action을 모아 놓은 배열로 이름과 함수를 맵핑
	 * 그 중 하나인 run_task를 따라가다 보면 내부에서 process_execute라는 함수를 통해 task에 대한 process를 생성
	 * 해당 동작은 USERPOG 매크로가 true일 때 실행됨으로 false일 때는 수행되지 않음
	 * 
	 * STEP 02.
	 * 현재는 run_task 대신 기본 실행 함수인 run_test 함수가 동작한다.
	 * run_test의 인자로 들어오는 test 배열에는 test 할 함수와 이름이 맵핑되어 있음.
	 * ㄴ static const struct test tests[] = { ... }
	 * 
	 * STEP 03.
	 * 그 중에 하나인 'test_alarm_multiple' 함수를 살펴 보면 내부적으로 test_sleep이라는 테스트 함수를 다시 호출한다.
	 * test_sleep(5, 7)을 호출한다고 하면 5는 생성할 thread의 개수이고, 7은 반복을 수행할 개수이다.
	 * 따라서 결과를 보면 thread가 0번부터 4번까지 나오고 7번씩 화면에 출력되는 것을 확인할 수 있다.
	 * 차이가 있다면 0번 thread는 딜레이가 10, 1번 thread는 딜레이가 20, 4번 thread는 딜레이가 50이 되는 구조
	 * 즉, test_sleep에서는 thread를 지정된 개수만 생성하고, 지정된 시간만큼 쉬고, 쉰 다음에 출력에 대한 lock을 얻어 그 값을 증가시키는 구조이다.
	 * 
	 */

	/* Finish up. */
	if (power_off_when_done)
		power_off ();
	thread_exit ();
}

/* Clear BSS */
static void
bss_init (void) {
	/* The "BSS" is a segment that should be initialized to zeros.
	   It isn't actually stored on disk or zeroed by the kernel
	   loader, so we have to zero it ourselves.

	   The start and end of the BSS segment is recorded by the
	   linker as _start_bss and _end_bss.  See kernel.lds. */
	extern char _start_bss, _end_bss;
	memset (&_start_bss, 0, &_end_bss - &_start_bss);
}

/* Populates the page table with the kernel virtual mapping,
 * and then sets up the CPU to use the new page directory.
 * Points base_pml4 to the pml4 it creates. */
static void
paging_init (uint64_t mem_end) {
	uint64_t *pml4, *pte;
	int perm;
	pml4 = base_pml4 = palloc_get_page (PAL_ASSERT | PAL_ZERO);

	extern char start, _end_kernel_text;
	// Maps physical address [0 ~ mem_end] to
	//   [LOADER_KERN_BASE ~ LOADER_KERN_BASE + mem_end].
	for (uint64_t pa = 0; pa < mem_end; pa += PGSIZE) {
		uint64_t va = (uint64_t) ptov(pa);

		perm = PTE_P | PTE_W;
		if ((uint64_t) &start <= va && va < (uint64_t) &_end_kernel_text)
			perm &= ~PTE_W;

		if ((pte = pml4e_walk (pml4, va, 1)) != NULL)
			*pte = pa | perm;
	}

	// reload cr3
	pml4_activate(0);
}

/* Breaks the kernel command line into words and returns them as
   an argv-like array. */
static char **
read_command_line (void) {
	static char *argv[LOADER_ARGS_LEN / 2 + 1];
	char *p, *end;
	int argc;
	int i;

	argc = *(uint32_t *) ptov (LOADER_ARG_CNT);
	p = ptov (LOADER_ARGS);
	end = p + LOADER_ARGS_LEN;
	for (i = 0; i < argc; i++) {
		if (p >= end)
			PANIC ("command line arguments overflow");

		argv[i] = p;
		p += strnlen (p, end - p) + 1;
	}
	argv[argc] = NULL;

	/* Print kernel command line. */
	printf ("Kernel command line:");
	for (i = 0; i < argc; i++)
		if (strchr (argv[i], ' ') == NULL)
			printf (" %s", argv[i]);
		else
			printf (" '%s'", argv[i]);
	printf ("\n");

	return argv;
}

/* Parses options in ARGV[]
   and returns the first non-option argument. */
static char **
parse_options (char **argv) {
	for (; *argv != NULL && **argv == '-'; argv++) {
		char *save_ptr;
		char *name = strtok_r (*argv, "=", &save_ptr);
		char *value = strtok_r (NULL, "", &save_ptr);

		if (!strcmp (name, "-h"))
			usage ();
		else if (!strcmp (name, "-q"))
			power_off_when_done = true;
#ifdef FILESYS
		else if (!strcmp (name, "-f"))
			format_filesys = true;
#endif
		else if (!strcmp (name, "-rs"))
			random_init (atoi (value));
		else if (!strcmp (name, "-mlfqs"))
			thread_mlfqs = true;
#ifdef USERPROG
		else if (!strcmp (name, "-ul"))
			user_page_limit = atoi (value);
		else if (!strcmp (name, "-threads-tests"))
			thread_tests = true;
#endif
		else
			PANIC ("unknown option `%s' (use -h for help)", name);
	}

	return argv;
}

/* Runs the task specified in ARGV[1]. */
static void
run_task (char **argv) {
	const char *task = argv[1];

	printf ("Executing '%s':\n", task);
#ifdef USERPROG
	if (thread_tests){
		run_test (task);
	} else {
		process_wait (process_create_initd (task));
	}
#else
	run_test (task);
#endif
	printf ("Execution of '%s' complete.\n", task);
}

/* Executes all of the actions specified in ARGV[]
   up to the null pointer sentinel. */
static void
run_actions (char **argv) {
	/* An action. */
	struct action {
		char *name;                       /* Action name. */
		int argc;                         /* # of args, including action name. */
		void (*function) (char **argv);   /* Function to execute action. */
	};

	/* Table of supported actions. */
	static const struct action actions[] = {
		{"run", 2, run_task},
#ifdef FILESYS
		{"ls", 1, fsutil_ls},
		{"cat", 2, fsutil_cat},
		{"rm", 2, fsutil_rm},
		{"put", 2, fsutil_put},
		{"get", 2, fsutil_get},
#endif
		{NULL, 0, NULL},
	};

	while (*argv != NULL) {
		const struct action *a;
		int i;

		/* Find action name. */
		for (a = actions; ; a++)
			if (a->name == NULL)
				PANIC ("unknown action `%s' (use -h for help)", *argv);
			else if (!strcmp (*argv, a->name))
				break;

		/* Check for required arguments. */
		for (i = 1; i < a->argc; i++)
			if (argv[i] == NULL)
				PANIC ("action `%s' requires %d argument(s)", *argv, a->argc - 1);

		/* Invoke action and advance. */
		a->function (argv);
		argv += a->argc;
	}

}

/* Prints a kernel command line help message and powers off the
   machine. */
static void
usage (void) {
	printf ("\nCommand line syntax: [OPTION...] [ACTION...]\n"
			"Options must precede actions.\n"
			"Actions are executed in the order specified.\n"
			"\nAvailable actions:\n"
#ifdef USERPROG
			"  run 'PROG [ARG...]' Run PROG and wait for it to complete.\n"
#else
			"  run TEST           Run TEST.\n"
#endif
#ifdef FILESYS
			"  ls                 List files in the root directory.\n"
			"  cat FILE           Print FILE to the console.\n"
			"  rm FILE            Delete FILE.\n"
			"Use these actions indirectly via `pintos' -g and -p options:\n"
			"  put FILE           Put FILE into file system from scratch disk.\n"
			"  get FILE           Get FILE from file system into scratch disk.\n"
#endif
			"\nOptions:\n"
			"  -h                 Print this help message and power off.\n"
			"  -q                 Power off VM after actions or on panic.\n"
			"  -f                 Format file system disk during startup.\n"
			"  -rs=SEED           Set random number seed to SEED.\n"
			"  -mlfqs             Use multi-level feedback queue scheduler.\n"
#ifdef USERPROG
			"  -ul=COUNT          Limit user memory to COUNT pages.\n"
#endif
			);
	power_off ();
}


/* Powers down the machine we're running on,
   as long as we're running on Bochs or QEMU. */
void
power_off (void) {
#ifdef FILESYS
	filesys_done ();
#endif

	print_stats ();

	printf ("Powering off...\n");
	outw (0x604, 0x2000);               /* Poweroff command for qemu */
	for (;;);
}

/* Print statistics about Pintos execution. */
static void
print_stats (void) {
	timer_print_stats ();
	thread_print_stats ();
#ifdef FILESYS
	disk_print_stats ();
#endif
	console_print_stats ();
	kbd_print_stats ();
#ifdef USERPROG
	exception_print_stats ();
#endif
}
