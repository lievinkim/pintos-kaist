#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

/* Proj 2-3. syscalls */
#include "threads/vaddr.h" 					// check_address
#include "userprog/process.h"				// process_fork, process_exit
#include "threads/palloc.h"					// PAL_ZERO

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

/* Project 2 */
void halt(void);
void exit(int status);
int write(int fd, const void *buffer, unsigned size);
tid_t fork(const char *thread_name, struct intr_frame *f);
int wait(tid_t child_tid);
int exec(char *file_name);
void check_address(uaddr);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f) {

	// printf ("system call!\n");

	switch (f->R.rax)
	{
	case SYS_HALT:
		halt();
		break;
	case SYS_EXIT:
		exit(f->R.rdi);
		break;
	case SYS_FORK:
		f->R.rax = fork(f->R.rdi, f);
		break;
	case SYS_EXEC:
		if (exec(f->R.rdi) == -1)
			exit(-1);
		break;
	case SYS_WAIT:
		f->R.rax = wait(f->R.rdi);
		break;
	case SYS_WRITE:
		f->R.rax = write(f->R.rdi, (char *) f->R.rsi, f->R.rdx);
		break;
	default:
		break;
	}
	
	// thread_exit 함수를 호출하면 시스템 콜 한번 호출 후 스레드 종료
	// 따라서 thread_exit을 주석 처리하고 필요한 경우 SYS_EXIT 시스템 콜 사용
	// thread_exit ();
}

/* P2-2. Syscalls */
/* Terminates PintOS by calling power_off(). No return */
void halt(void) {
	power_off();
}
/* End current thread, record exit status. No return. */
void exit(int status) {

	/* Proj 2-3. wait syscall */
	struct thread *cur = thread_current();					// 현재 돌아가는 스레드 정보 가져오기
	cur->exit_status = status;								// exit_status에 status 값 넣기
	printf("%s: exit(%d)\n", thread_name(), status);

	thread_exit();
}
/* Writes size bytes from buffer to the open file fd. */
/* Returns the number of bytes actually written, or -1 if the file could not be written */
int write(int fd, const void *buffer, unsigned size) {
	putbuf(buffer, size);
	return 0;
}
/* (parent) Returns pid of child on success or -1 on fail */
/* (child) Returns 0 */
tid_t fork(const char *thread_name, struct intr_frame *f)
{
	return process_fork(thread_name, f);
}
/* Parent는 Child 프로세스가 종료될 때까지 대기 (Child 프로세스가 exit_status 반환까지) */
int wait(tid_t child_tid) {
	return process_wait(child_tid);
}
/* Current Process에서 새로운 실행 가능한('executable') 파일을 실행하기 위함 */
/* open과 exec의 차이 : open은 txt, executable 등 어떤 파일이든 여는 것이며, exec은 executable을 실행 */
/* 성공 시 Return하지 않음 (실패 시 -1 리턴) */
int exec(char *file_name)
{
	struct thread *cur = thread_current();			// 노트. 현재 스레드 정보 가져오기
	check_address(file_name);						// 노트. 유효한 주소인지 확인

	/* 여기서 중요한 이슈 하나가 발생 -> process_exec 내 process_cleanup 함수로 인하여 f->R.rdi가 날아감 */
	/* 따라서 filename을 동적 할당하여 복사한 후 넘겨줘야함 */

	int siz = strlen(file_name)+1;
	char *fn_copy = palloc_get_page(PAL_ZERO);

	if (fn_copy == NULL)
		exit(-1);

	strlcpy(fn_copy, file_name, siz);
	if (process_exec(fn_copy) == -1)
		return -1;

	NOT_REACHED();
	return 0;
}




/* P2-2. Check Address */
// 1. Null pointer
// 2. A pointer to kernel virtual address space (above KERN_BASE)
// 3. A pointer to unmapped virtual memory (causes page_fault)
void check_address(const uint64_t *uaddr) {
	struct thread *cur = thread_current();
	if (uaddr == NULL || !(is_user_vaddr(uaddr)) || pml4_get_page(cur->pml4, uaddr) == NULL)
	{
		exit(-1);
	}
}

