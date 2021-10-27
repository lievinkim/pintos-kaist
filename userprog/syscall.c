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

/* Proj 2-4. file descriptor */
#include "filesys/filesys.h"
#include "filesys/file.h"
#include <list.h>

/* Project 3. check adddress, writable 수정에 따른 헤더 추가 */
#include "vm/file.h"

/* Proj 2-7. Extra */
/* stdin, stdout 상수 선언 */
const int STDIN = 1;
const int STDOUT = 2;

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

/* Project 3. check adddress, writable 수정에 따른 선언 추가 */
void check_writable_addr(const uint64_t *uaddr);

bool create(const char *file, unsigned initial_size);
bool remove(const char *file);
int open(const char *file);
int filesize(int fd);
int read(int fd, void *buffer, unsigned size);
void seek(int fd, unsigned position);
unsigned tell(int fd);
void close(int fd);

/* Proj 2-7. Extra */
int dup2(int oldfd, int newfd);

/* Project 3. MMF : mmap, munmap 함수 선언 */
static void *mmap (void *addr, size_t length, int writable, int fd, off_t offset);
static void munmap (void* addr);

int add_file_to_fdt(struct file *file);
static struct file *find_file_by_fd(int fd);
void remove_file_from_fdt(int fd);

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

	/* Proj 2-4. file descriptor - race condition을 막기 위한 장치 */
	lock_init(&filesys_lock);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f) {

	/* Project 3. SG : 스택 포인터 저장 */
#ifdef VM
	struct thread* curr = thread_current ();
	curr->saving_rsp = f->rsp;
#endif
	
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
		/* exec 함수를 조금 다듬기 - 특별한 이유 없음 */
		f->R.rax = exec(f->R.rdi);
		break;
	case SYS_WAIT:
		f->R.rax = wait(f->R.rdi);
		break;
	case SYS_CREATE:
		f->R.rax = create(f->R.rdi, f->R.rsi);
		break;
	case SYS_REMOVE:
		f->R.rax = remove(f->R.rdi);
		break;
	case SYS_OPEN:
		f->R.rax = open(f->R.rdi);
		break;
	case SYS_FILESIZE:
		f->R.rax = filesize(f->R.rdi);
		break;
	case SYS_READ:
		f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
		break;
	case SYS_WRITE:
		f->R.rax = write(f->R.rdi, (char *) f->R.rsi, f->R.rdx);
		break;
	case SYS_SEEK:
		seek(f->R.rdi, f->R.rsi);
		break;
	case SYS_TELL:
		f->R.rax = tell(f->R.rdi);
		break;
	case SYS_CLOSE:
		close(f->R.rdi);
		break;
	case SYS_DUP2:
		f->R.rax = dup2(f->R.rdi, f->R.rsi);
		break;

	/* Project 3. MMF : 시스템 콜 추가 */
	case SYS_MMAP:
		f->R.rax = (uint64_t) mmap ((void*) f->R.rdi, (size_t) f->R.rsi, (int) f->R.rdx, (int) f->R.r10, (off_t) f->R.r8);
		break;
	case SYS_MUNMAP:
		munmap (f->R.rdi);
		break;

	default:
		exit(-1);
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

	/* Proj 2-5. Process Termination Message */ 
	/* Project 3. AP : 주석 처리 -> process_exit으로 이동 */
	// printf("%s: exit(%d)\n", thread_name(), status);		// exit을 통해 혹은 다른 이유로 사용자 프로세스가 종료되었을 때 프로세스 이름과 exit 코드 출력

	thread_exit();
}
/* Writes size bytes from buffer to the open file fd. */
/* Returns the number of bytes actually written, or -1 if the file could not be written */
int write(int fd, const void *buffer, unsigned size) {
	check_address(buffer);
	int ret;

	struct file *fileobj = find_file_by_fd(fd);				// fd 정보를 통해 file 객체 가져오기
	if (fileobj == NULL)
		return -1;
	struct thread *cur = thread_current();					// 현재 실행 중인 스레드 정보 가져오기
	
	if (fileobj == STDOUT)												// fd가 1(fileobj=2), 즉, STDOUT이 들어왔을 때 바로 출력
	{
		if (cur->stdout_count == 0)										// dup2 대응하기 위함
		{
			// Not reachable
			NOT_REACHED();
			remove_file_from_fdt(fd);									// 잘못 저장된 파일임으로 삭제 처리
			ret = -1;			
		}
		else
		{
			putbuf(buffer, size);
			ret = size;
		}
	}
	else if (fileobj == STDIN)											// fd가 0(fileobj=1), 즉, STDIN이 들어왔을 때 -1로 넘김
	{
		ret = -1;
	}
	else
	{	
		lock_acquire(&filesys_lock);							// 글로벌 read/write lock acquire
		ret = file_write(fileobj, buffer, size);				// file system 함수 활용하여 쓰기 진행
		lock_release(&filesys_lock);							// 글로벌 read/write lcok release
	}

	return ret;
}
/* (parent) Returns pid of child on success or -1 on fail */
/* (child) Returns 0 */
tid_t fork(const char *thread_name, struct intr_frame *f)
{
	/* Project 3. Check address 수정 */
	check_address((uint64_t *)thread_name);
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

	if (uaddr == NULL || !(is_user_vaddr(uaddr))) {
		exit(-1);
	}

	/* Project 3. AP : Page Fault 처리 변경 */
	uint64_t *pte = pml4e_walk(thread_current()->pml4, (const uint64_t) uaddr, 0);
	if (pte == NULL) exit(-1);

	struct page *page = spt_find_page(&thread_current()->spt, uaddr);
	if (page == NULL) exit(-1);

	/* Project 2 내용으로 주석 처리 */
	// else if (pml4_get_page(cur->pml4, uaddr) == NULL) {
	// 	printf("page is null\n\n");
	// 	exit(-1);
	// }
}

/* Project 3. AP : 추가 */
void check_writable_addr(const uint64_t *uaddr){
	struct page *page = spt_find_page (&thread_current()->spt, uaddr);
	if (page == NULL || !page->writable) exit(-1);
}



/* Proj 2-4. file descriptor 함수 */
// Creates a new file called file initially initial_size bytes in size.
// Returns true if successful, false otherwise
bool create(const char *file, unsigned initial_size) {
	check_address(file);
	bool success;
	
	// lock_acquire(&filesys_lock);
	success = filesys_create(file, (off_t)initial_size); // file system 함수 활용하여 생성
	// lock_release(&filesys_lock);

	return success;
}
// Deletes the file called 'file'. Returns true if successful, false otherwise.
bool remove(const char *file) {
	check_address(file);
	bool success;

	lock_acquire(&filesys_lock);
	success = filesys_remove(file); // file system 함수 활용하여 삭제
	lock_release(&filesys_lock);

	return success;
}
// Opens the file called file, returns fd or -1 (if file could not be opened for some reason)
int open(const char *file) {
	check_address(file);
	struct file *fileobj;
	
	lock_acquire(&filesys_lock);
	fileobj = filesys_open(file);  // file system 함수 활용하여 open
	lock_release(&filesys_lock);

	if (fileobj == NULL) {	// obj 생성 여부 확인
		return -1;
	} 						
		
	int fd = add_file_to_fdt(fileobj); 			// 생성된 obj는 프로세스의 fdt에 추가 및 fd 리턴 받음
	
	if (fd == -1)								// fdt가 가득 찼는지 여부 확인
		file_close(fileobj);

	return fd;
}
// Returns the size, in bytes, of the file open as fd.
int filesize(int fd) {
	int length;

	struct file *fileobj = find_file_by_fd(fd);
	if (fileobj == NULL)
		return -1;

	lock_acquire(&filesys_lock);
	length = file_length(fileobj);
	lock_release(&filesys_lock);

	return length;
}
// Reads size bytes from the file open as fd into buffer.
// Returns the number of bytes actually read (0 at end of file), or -1 if the file could not be read
int read(int fd, void *buffer, unsigned size)
{
	check_address(buffer);

	/* Project 3. AP : buffer에 쓰기 가능한 주소인지 검사 */
	check_writable_addr(buffer);

	int ret;

	struct file *fileobj = find_file_by_fd(fd);			// fd 정보를 통해 file 객체 가져오기
	if (fileobj == NULL)
		return -1;
	struct thread *cur = thread_current();				// 현재 실행 중인 스레드 정보 가져오기

	if (fileobj == STDIN)											// fd가 0(fileobj=1), 즉 STDIN이 들어올 때 바로 읽기
	{
		if(cur->stdin_count == 0)									// dup2 대응하기 위함
		{
			// Not reachable
			NOT_REACHED();
			remove_file_from_fdt(fd);								// 잘못 저장된 파일임으로 삭제 처리
			ret = -1;
		}
		else {
			int i;
			unsigned char *buf = buffer;
			for (i = 0; i < size; i++)
			{
				char c = input_getc();
				*buf++ = c;
				if (c == '\0')
					break;
			}
			ret = i;
		}
	}
	else if (fileobj == STDOUT)										// fd가 1(fileobj=2), 즉 STDOUT이 들어올 때 -1로 넘김
	{
		ret = -1;
	}
	else													// fd가 0, 1 외에 값이 들어올 때
	{
		lock_acquire(&filesys_lock);						// 글로벌 read/write lock acquire
		ret = file_read(fileobj, buffer, size);				// file system 함수 활용하여 읽기 진행
		lock_release(&filesys_lock);						// 글로벌 read/write lcok release
	}

	return ret;
}
// Changes the next byte to be read or written in open file fd to position,
// expressed in bytes from the beginning of the file (Thus, a position of 0 is the file's start).
void seek(int fd, unsigned position)
{
	lock_acquire(&filesys_lock);
	struct file *fileobj = find_file_by_fd(fd);
	if (fileobj <= 2)
	{
		lock_release(&filesys_lock);
		return;
	}
	fileobj->pos = position;
	lock_release(&filesys_lock);
}
// Returns the position of the next byte to be read or written in open file fd, expressed in bytes from the beginning of the file.
unsigned tell(int fd)
{
	unsigned position;
	lock_acquire(&filesys_lock);
	struct file *fileobj = find_file_by_fd(fd);
	if (fileobj <= 2)
	{
		lock_release(&filesys_lock);
		return;
	}
	position = file_tell(fileobj);
	lock_release(&filesys_lock);

	return position;
}
// Closes file descriptor fd. Ignores NULL file. Returns nothing.
void close(int fd)
{

	struct file *fileobj = find_file_by_fd(fd);					// fd 정보를 통해 file 정보 가져오기
	
	if (fileobj == NULL)										// 객체 유효성 검사
	{
		return;
	}
	struct thread *cur = thread_current();						// 현재 실행 중인 스레드 정보 가져오기

	/* Proj 2-7. Extra */
	/* stdin, stdout close 요청 왔을 때 count 1씩 감소 */
	if (fd == 0 || fileobj == STDIN)
	{
		cur->stdin_count--;
	}
	else if (fd == 1 || fileobj == STDOUT)
	{
		cur->stdout_count--;
	}

	remove_file_from_fdt(fd);									// fd 정보를 통해 file을 fdt에서 제거

	if (fd <= 1 || fileobj <= 2)
	{
		return;
	}

	
	/* Proj 2-7. Extra */
	/* dup_Count가 0일 때만 삭제할 수 있으며 1 이상인 경우에는 1씩 감소 */
	if (fileobj->dupCount == 0)									
		file_close(fileobj);
	else
		fileobj->dupCount--;

}
/* Proj 2-7. Extra */
// Creates 'copy' of oldfd into newfd. If newfd is open, close it. Returns newfd on success, -1 on fail (invalid oldfd)
// After dup2, oldfd and newfd 'shares' struct file, but closing newfd should not close oldfd (important!)
int dup2(int oldfd, int newfd)
{
	struct file *fileobj = find_file_by_fd(oldfd);			// oldfd에 대한 fileobj 가져오기
	if (fileobj == NULL)									// 유효성 검사
		return -1;

	struct file *deadfile = find_file_by_fd(newfd);			// newfd에 대한 fileobj 가져오기

	if (oldfd == newfd)										// oldfd와 newfd가 같을 경우 조치 필요 없음
		return newfd;

	struct thread *cur = thread_current();					// 현재 실행 중인 스레드 정보 가져오기
	struct file **fdt = cur->fdTable;						// 실행 중인 스레드의 FDT 정보 가져오기

	// Don't literally copy, but just increase its count and share the same struct file
	// [syscall close] Only close it when count == 0

	// Copy stdin or stdout to another fd
	if (fileobj == STDIN)									// 우선 복사하고자 하는 대상을 파악하고 해당 되는 count 1씩 증가
		cur->stdin_count++;
	else if (fileobj == STDOUT)
		cur->stdout_count++;
	else
		fileobj->dupCount++;

	close(newfd);											// newfd의 값을 NULL로 초기화
	fdt[newfd] = fileobj;									// newfd에 복제 대상 대입
	return newfd;
}


/* Proj 2-4. file descriptor 서브 함수 */
// Find open spot in current thread's fdt and put file in it. Returns the fd.
int add_file_to_fdt(struct file *file)
{
	struct thread *cur = thread_current();					// 현재 실행 중인 스레드 구조체 정보 가져오기
	struct file **fdt = cur->fdTable; 						// 스레드의 fdTable을 입력

	while (cur->fdIdx < FDCOUNT_LIMIT && fdt[cur->fdIdx])	// 해당 fdIdx 값이 NULL이 아닌 경우 다음 fdIdx에 추가하기
		cur->fdIdx++;

	if (cur->fdIdx >= FDCOUNT_LIMIT)						// FDT에 공간 있는지 확인
		return -1;

	fdt[cur->fdIdx] = file;									// fdIdx에 file 정보 입력
	return cur->fdIdx;										// fdIdx 리턴
}
// Check if given fd is valid, return cur->fdTable[fd]
static struct file *find_file_by_fd(int fd)
{
	struct thread *cur = thread_current();			// 현재 실행 중인 스레드 구조체 정보 가져오기  

	// Error - invalid fd
	if (fd < 0 || fd >= FDCOUNT_LIMIT)				// fd 유효성 검사
		return NULL;

	return cur->fdTable[fd]; 						// file 전송 (만약 비어 있을 경우 자동으로 NULL 리턴)
}
// Check for valid fd and do cur->fdTable[fd] = NULL. Returns nothing
void remove_file_from_fdt(int fd)
{
	struct thread *cur = thread_current();			// 현재 실행 중인 스레드 구조체 정보 가져오기

	// Error - invalid fd
	if (fd < 0 || fd >= FDCOUNT_LIMIT)				// 전달 받은 fd 값 유효성 검사 (should be 0 <= fd < FDCOUNT_LIMIT)
		return;

	cur->fdTable[fd] = NULL;						// 해당 FDT 값을 NULL로 바꿈
}

/* Project 3. MMF : mmap 함수와 munmap 함수 구현하기 */
/*
 * addr : 할당한 가상 주소
 * length : 할당할 파일의 길이
 * fd : 파일
 * writable : 메모리 쓰기 가능 여부
 * offset : 파일 내 할당 시작 위치
 */
static void *
mmap (void *addr, size_t length, int writable, int fd, off_t offset){

	/* 모든 파라미터 에러를 핸들한 후 do_mmap 호출 */
	/* Handle all parameter error and pass it to do_mmap */

	if (addr == 0 || (!is_user_vaddr(addr))) return NULL;							// 주소는 사용자 가상 공간이어야 하고 0이면 안됨
	if (length == 0) return NULL;													// length가 0이면 안됨
	if ((uint64_t)addr % PGSIZE != 0) return NULL;									// addr은 page-aligned 되어 있어야 함
	if (offset % PGSIZE != 0) return NULL;											// offset은 page-aligned 되어 있어야 함
	if ((uint64_t)addr + length == 0) return NULL;									// addr와 length 둘 다 0이면 안됨 (?)
	if (!is_user_vaddr((uint64_t)addr + length)) return NULL;						// addr와 length의 합이 사용자 영역에 있어야 함
	for (uint64_t i = (uint64_t) addr; i < (uint64_t) addr + length; i += PGSIZE){
		if (spt_find_page (&thread_current()->spt, (void *)i)!=NULL) return NULL;	// spt에서 할당된 페이지를 찾지 못하면 안됨
	}

	struct file *target = process_get_file(fd);										// fd 인자를 기반으로 파일 탐색 시작
	if (target == NULL) return NULL;												// 파일 탐색 실패 시 NULL 리턴
	

	return do_mmap(addr, length, writable, target, offset);							// do_mmap 호출!
}

static void
munmap (void* addr){
	do_munmap(addr);																// 맵핑 정보 해제
}