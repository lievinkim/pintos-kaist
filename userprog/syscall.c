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
#include "userprog/process.h"				// process_fork

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

/* Project 2 */
void halt(void);
void exit(int status);
int write(int fd, const void *buffer, unsigned size);
tid_t fork(const char *thread_name, struct intr_frame *f);
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

	printf ("system call!\n");

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

