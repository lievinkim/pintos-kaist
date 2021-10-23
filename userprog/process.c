#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);

/* General process initializer for initd and other process. */
static void
process_init (void) {
	struct thread *current = thread_current ();
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

	/* P2-1. 프로그램명 추출하기 위한 코드
	 * file_name의 주소가 시작점이며 첫 빈칸을 만난 다음 위치 주소를 save_ptr에 저장
	 * 빈칸은 strtok_r 함수에 의해 NULL 값으로 변경됨
	 * 따라서 file_name을 thread_create에 넣으면 NULL 전까지 읽음으로 프로그램명만 전달하는 효과가 발생
	 */ 
	char *save_ptr;
	strtok_r(file_name, " ", &save_ptr);

	/* Create a new thread to execute FILE_NAME. */
	/* thread_create 설명 
	 * - file_name : 스레드 이름 (문자열)
	 * - PRI_DEFAULT : 스레드 우선순위 (31)
	 * - initd : 생성된 스레드가 실행할 함수를 가리키는 포인터 (start_process)
	 * - fn_copy : initd 함수를 수행할 때 사용하는 인자 값
	 */
	tid = thread_create (file_name, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
		palloc_free_page (fn_copy);
	return tid;
}

/* A thread function that launches first user process. */
static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();

	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t
process_fork (const char *name, struct intr_frame *if_) {

	/* Proj 2-3. fork syscall */
	/* Clone current thread to new thread.*/
	struct thread *cur = thread_current(); 							// 노트. 현재 스레드 구조체 가져오기 (부모 스레드)
	memcpy(&cur->parent_if, if_, sizeof(struct intr_frame));		// 노트. 현재 스레드 parent_if에 값 저장 (부모 스레드) -> 향후 해당 정보를 child에 전달

	tid_t tid = thread_create(name, PRI_DEFAULT, __do_fork, cur);   // 노트. child 프로세스 스레드 생성 (__do_fork 함수 실행 및 cur를 인자로 전달)
	if (tid == TID_ERROR)
		return TID_ERROR;

	/* Proj 2-3. wait syscall */
	struct thread *child = get_child_with_pid(tid);					// 노트. tid로 child 스레드 정보 가져오기
	sema_down(&child->fork_sema); 									// 노트. child가 로드 될 때까지 대기 (로드 되면 sema_up을 차일드가 해줌)
	if (child->exit_status == -1)									// 노트. 만약 가져온 child의 exit_status 값이 -1이라면 에러
		return TID_ERROR;

	return tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	if (is_kernel_vaddr(va))
	{
		return true; 										// 노트. false 리턴 시 pml4_for_each가 종료되기 때문에 true를 통해 해당 커널 va 전달
	}
	else
	{
#ifdef DEBUG
		printf("[fork-duplicate] pass at step 1 %llx\n", va);
#endif		
	}

#ifdef DEBUG
	printf("Is user %d, is kernel %d, writable %d\n", is_user_pte(pte), is_kern_pte(pte), is_writable(pte));
#endif

	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page (parent->pml4, va);
	if (parent_page == NULL)
	{
#ifdef DEBUG
		printf("[fork-duplicate] pass at step 1 %llx\n", va);
#endif	
		return false;										// 노트. parent_page가 Null인 경우 false 리턴
	}

#ifdef DEBUG
	/* page table, virtual address 이해하기 */
	/* pte는 하나의 page table entry를 포인팅 하는 주소를 의미함 */
	/* *pte = page table entry = address of the physical frame */
	void *test = ptov(PTE_ADDR(*pte)) + pg_ofs(va); 	// should be same as parent_page -> Yes!
	uint64_t va_offset = pg_ofs(va);					// should be 0; va comes from PTE, so there must be no 12bit physical offset
#endif

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	newpage = palloc_get_page(PAL_USER);
	if (newpage == NULL)
	{
#ifdef DEBUG
		printf("[fork-duplicate] failed to palloc new page\n");
#endif	
		return false;										// 노트. newpage가 Null인 경우 false 리턴
	}

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	memcpy(newpage, parent_page, PGSIZE);					// 노트. parent_page를 newpage에 복사
	writable = is_writable(pte); 							// 노트. *PTE is an address that points to parent_page

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: if fail to insert page, do error handling. */
#ifdef DEBUG
		printf("Failed to map user virtual page to given physical frame\n"); // #ifdef DEBUG
#endif
		return false;
	}

#ifdef DEBUG
	/* 'va'가 newpage에 잘 맞게 맵핑 되었는지 확인 */
	if (pml4_get_page(current->pml4, va) != newpage)
		printf("Not mapped!"); // never called

	printf("--Completed copy--\n");
#endif

	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork (void *aux) {
	struct intr_frame if_;
	struct thread *parent = (struct thread *) aux;
	struct thread *current = thread_current ();
	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
	struct intr_frame *parent_if;

	/* Proj 2-3. fork syscall */
	parent_if = &parent->parent_if; 						// 노트. 새로 만든 intr_frame 구조체에 부모의 parent_if 값 저장
	
	bool succ = true;

	/* 1. Read the cpu context to local stack. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));
	/* Proj 2-3. fork syscall */	
	if_.R.rax = 0; 											// 노트. fork 시 child의 리턴 값을 0으로 세팅

	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/

	/* Proj2-6. file descriptor */
	if (parent->fdIdx == FDCOUNT_LIMIT)  	// 제대로 복사되었는지 확인
		goto error;

	/* Proj 2-7. Extra */
	/* 같은 파일을 여러 fd들이 공유하는 경우 이러한 관계를 복사하기 위해 associative map 사용 (dict, hashmap 등) */
	/* multi-oom과 같은 테스트 케이스들은 해당 특징을 필요로 하지 않음 */
	const int MAPLEN = 10;
	struct MapElem map[10]; 	// MapElem에는 key와 value 존재 (key-parent's struct file * , value-child's newly created struct file *)
	int dupCount = 0;			// 맵을 채우기 위한 인덱스 역할

	for (int i = 0; i < FDCOUNT_LIMIT; i++) 			// 복사한 FDT의 크기 만큼 하나씩 살펴보기
	{
		struct file *file = parent->fdTable[i];			// 부모의 i번째 인덱스 파일 가져오기
		if (file == NULL)								// 유효성 체크
			continue;

		/* Proj 2-7. Extra */
		/* key-pair 배열에 대한 선형 검색 시작 */
		/* child에 이미 복사된 'file'이라면, 더 이상 복사하지 않고 공유하는 것이 핵심 */
		bool found = false;								// map에 있는 지 여부 확인 (기본값 false)

		for (int j = 0; j < MAPLEN; j++)				// 인덱스 0부터 MAPLEN 만큼 loop 진행
		{
			if (map[j].key == file)                     // 인덱스 j의 key 값이 file인 경우 (이미 복사된 파일이라는 뜻)
			{
				found = true;							// found는 true로 변경
				current->fdTable[i] = map[j].value;		// 인덱스 j의 value 값을 FDT에 넣어줌
				break;
			}
		}

		if (!found)										// 만약 찾지 못했다면 (복사된 적 없는 파일이라면)
		{
			struct file *new_file;						// 복사할 파일을 담을 new_file 변수 생성 
			if (file > 2)								// i가 2보다 작으면 STDIN, STDOUT이기 때문
				new_file = file_duplicate(file);		// i가 2보다 큰 경우에는 file system 함수를 활용하여 복사
			else
				new_file = file;

			current->fdTable[i] = new_file;				// fork된 child 스레드의 fdt에 추가
			if (dupCount < MAPLEN)						// 인덱스가 MAPLEN 보다 작다면
			{
				map[dupCount].key = file;				// 해당 인덱스의 key 값에 file 넣고
				map[dupCount++].value = new_file;		// 해당 인덱스의 value 값에 new_file 넣고 1 증가
			}
		}		
	}
	current->fdIdx = parent->fdIdx;					// child와 부모의 fdIdx 값 동일하게 맞추기 

	// process_init ();										// 노트. 현재까지 큰 의미는 없어 보이는 함수
	
	/* Proj 2-3. fork syscall */
	sema_up(&current->fork_sema); 							// 노트. child load 성공 및 fork 작업 끝났음을 부모에게 전달

	/* Finally, switch to the newly created process. */
	if (succ)
		do_iret (&if_);
error:

	/* Proj 2-3. wait syscall */
	current->exit_status = TID_ERROR;						// 노트. 생성 실패 시에는 exit_status에 오류 값을 넣어주고 바로 전달
	
	/* Proj 2-3. fork syscall */
	sema_up(&current->fork_sema); 							// 노트. child load 실패 및 fork 작업 끝났음을 부모에게 전달
	exit(TID_ERROR);										// 노트. TID_ERROR 전달
	// thread_exit ();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* We first kill the current context */
	process_cleanup ();

	/* P2-1. Parsing
	 * argv[]는 인자를 저장 할 배열
	 * argc는 인자의 개수를 세는 카운터
	 * token, save_ptr은 parsing에 필요한 인자
	 * echo hello를 예시로 든다면,
	 */
	char *argv[30];
	int argc = 0;

	char *token, *save_ptr;
	token = strtok_r(file_name, " ", &save_ptr); // echo hello 사이 빈 칸에 \0 입력하여 token에는 echo\0 저장
	while (token != NULL) // 1) token에는 echo\0이 저장되어 안으로 들어감
	{
		argv[argc] = token; // argv[0]에 echo\0 저장
		token = strtok_r(NULL, " ", &save_ptr); // NULL을 넣으면 시작 주소가 &save_ptr가 되며, token에는 hello\0 저장
		argc++; // argc는 1로 올라감 -> 그 후에 다시 while 문 한번 더 돌고 종료
	}
	// 결과적으로 argv[0] = echo\0, argv[1] = hello\0 저장
	// argc는 2 저장

	/* And then load the binary */
	success = load (file_name, &_if);

	/* If load failed, quit. */
	if (!success)
	{
		palloc_free_page(file_name);
		return -1;
	}


    /* P2-1. Load arguments onto the USER_STACK */
	void **rspp = &_if.rsp; // interupt frame의 stack pointer
	argument_stack(argv, argc, rspp); // argv, argc, rspp 전달
	_if.R.rdi = argc; // 결과물 저장 (rdi에 인자 개수)
	_if.R.rsi = (uint64_t)*rspp + sizeof(void *); // 결과물 저장 (rsi에 argv[0] 값 저장 주소 포인터)

	/* Proj 2-1. Debugging */
	// hex_dump(_if.rsp, _if.rsp, USER_STACK - (uint64_t)*rspp, true);
	palloc_free_page(file_name);

	/* Start switched process. */
	do_iret (&_if);
	NOT_REACHED ();
}

/* Load user stack with arguments
 * argv 배열, argc 정수, rspp 스택 포인터를 인자로 전달
 */

void argument_stack(char **argv, int argc, void **rspp)
{
	// 1. Save argument strings (character by character)
	// 인자의 개수 만큼 for문 (i)
	// 첫 번째 인자의 길이를 구하고, 스택 포인터를 -1씩 감소하면서 한 글자씩 저장
	// 그리고 argv에 해당 인자가 저장된 주소 값을 저장
	for (int i = argc - 1; i >= 0; i--)
	{
		int N = strlen(argv[i]);
		for (int j = N; j >= 0; j--)
		{
			char individual_character = argv[i][j];
			(*rspp)--;
			**(char **)rspp = individual_character; // 1 byte
		}
		argv[i] = *(char **)rspp; // push this address too
	}

	// 2. Word-align padding
	// rspp를 8로 나누어 나머지를 pad에 저장
	// pad 만큼 스택 포인터를 -1씩 감소하면서 0을 입력
	// 8의 배수를 맞춰주기 위함
	int pad = (int)*rspp % 8;
	for (int k = 0; k < pad; k++)
	{
		(*rspp)--;
		**(uint8_t **)rspp = (uint8_t)0; // 1 byte
	}

	// 3. Pointers to the argument strings
	// char 포인터 사이즈를 구하고 PTR_SIZE에 값 저장
	size_t PTR_SIZE = sizeof(char *);

	// NULL Pointer Sentinel 생성 (0값으로 저장)
	(*rspp) -= PTR_SIZE;
	**(char ***)rspp = (char *)0;

	// 위에서 저장했던 인자의 주소값을 스택 포인터를 PTR_SIZE씩 감소하면서 값 저장
	for (int i = argc - 1; i >= 0; i--)
	{
		(*rspp) -= PTR_SIZE;
		**(char ***)rspp = argv[i];
	}

	// 4. Return address
	// 호출 함수의 다음 명령어를 수행할 수 있도록 다음 명령어에 대한 주소 저장
	// 여기서는 fake address인 0을 저장
	(*rspp) -= PTR_SIZE;
	**(void ***)rspp = (void *)0;
}


/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int
process_wait (tid_t child_tid UNUSED) {
	/* XXX: Hint) The pintos exit if process_wait (initd), we recommend you
	 * XXX:       to add infinite loop here before
	 * XXX:       implementing the process_wait. */
	// for (int i = 0; i < 1000000000; i++); // 임시방편용
	
	/* Proj 2-2. wait syscall */
	struct thread *cur = thread_current(); 					// 노트. 현재 실행 중인 스레드 가져오기 (부모 스레드)
	struct thread *child = get_child_with_pid(child_tid);	// 노트. get_child_with_pid 통해 child 스레드 가져오기

	if (child == NULL)										// 노트. get_child_with_pid 통해 못 가져온 경우 (내 child가 아님)
		return -1;

	sema_down(&child->wait_sema); 							// 노트. child 프로세스가 실행 후 sema_up 할 때까지 기다림
	int exit_status = child->exit_status; 					// 노트. child 프로세스 종료 후의 exit_status 값 저장

	list_remove(&child->child_elem);						// 노트. child 리스트에서 child 삭제
	sema_up(&child->free_sema);								// 노트. child 회수 (wake-up child in process_exit - proceed with thread_exit)

	return exit_status;
}

/* 전달 받은 pid로 child_list에서 child를 찾고 해당 스레드 전달 */
struct thread *get_child_with_pid(int pid)
{
	struct thread *cur = thread_current();					// 노트. 현재 실행 중인 스레드 가져오기 (부모 스레드)
	struct list *child_list = &cur->child_list;				// 노트. 현재 실행 중인 스레드의 child_list 가져오기

	for (struct list_elem *e = list_begin(child_list); e != list_end(child_list); e = list_next(e))		// 노트. 탐색 시작
	{
		struct thread *t = list_entry(e, struct thread, child_elem);									// 노트. child_elem 통해 스레드 정보 가져오기
		if (t->tid == pid)																				// 노트. 가져온 스레드의 tid 값이 pid와 동일한지 확인 (동일하면 child)
			return t;
	}
	return NULL;																						// 노트. pid와 동일한 t를 못 찾은 경우 NULL 리턴
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *cur = thread_current ();
	/* TODO: Your code goes here.
	 * TODO: Implement process termination message (see
	 * TODO: project2/process_termination.html).
	 * TODO: We recommend you to implement process resource cleanup here. */

	/* Proj 2-4. file descriptor - 닫는 부분 */
	for (int i=0; i<FDCOUNT_LIMIT; i++)				// FDCOUNT_LIMIT 만큼 읽고 해당 되는 곳을 하나씩 close
	{
		close(i);
	}
	// palloc_free_page(cur->fdTable);				// 하나씩 close 후 thread_create에서 할당 받은 페이지 free (FDT 초기화)
	palloc_free_multiple(cur->fdTable, FDT_PAGES); 	// multi-oom

	/* Proj 2-6. Denying write to executable - 프로세스 종료 시 쓰기 가능 상태로 변경 */
	/* 예시. 프로세스 시작 시, e 파일을 로드 하면서 cur->running에 args-none 추가 */
	/* 프로세스 종료 시, cur->running에 있던 args-none을 제거 (위에 close는 FDT 파일에 있는 애들이 대상) */  
	file_close(cur->running);

	process_cleanup ();

	/* Proj 2-3. wait syscall */
	sema_up(&cur->wait_sema);					// 대기 중인(blocked) 부모 프로세스가 깨어날 수 있도록 함
	sema_down(&cur->free_sema);					// 부모 프로세스가 exit_status 값을 가질 때까지 child 프로세스 종료 지연 
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create (); // 노트. 페이지 디렉토리 생성
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ()); // 노트. 페이지 테이블 활성화

	/* Open executable file. */
	file = filesys_open (file_name); // 프로그램 파일 Open
	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		goto done;
	}

	/* Proj 2-6. Denying write to executable - 로드 시 쓰기 거부 상태로 변경 */
	t->running = file;
	file_deny_write(file);

	/* Read and verify executable header. */
	/* 노트. ELF 파일의 헤더 정보를 읽어와 저장 */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. */
	/* 배치 정보를 읽어와 저장 */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* Set up stack. */
	/* 스택 초기화 */
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */

	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	// file_close (file);	// 현재 running 중인 파일은 process exit에서 종료
	return success;
}

/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

/* Project 3. AP : VM 및 Lazy Load를 위한 lazy load segment 함수 구현 */
static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */														// 파일로부터 세그먼트 로드
	/* TODO: This called when the first page fault occurs on address VA. */							// VA에서 첫 페이지 폴트 발생 시 호출
	/* TODO: VA is available when calling this function. */											// 즉, VA는 존재할 수 밖에 없음

	struct load_info* li = (struct load_info *) aux;												// 읽어야 할 파일 정보 가져오기
	if (page == NULL)  return false;																// 페이지가 NULL 이라면 false 리턴
	ASSERT(li->page_read_bytes <= PGSIZE);															// 읽어야 할 바이트 수는 항상 PGSIZE 이하
	ASSERT(li-> page_zero_bytes <= PGSIZE);

	if (li->page_read_bytes > 0) {																	// 읽어야 할 바이트 수가 있다면
		file_seek (li->file, li->ofs);																// file 내 offset 찾기
		if (file_read (li->file, page->va, li->page_read_bytes) != (off_t)li->page_read_bytes) {	// 실제로 읽은 바이트 길이와 읽어야 할 바이트 길이 체크
			vm_dealloc_page (page);																	// 같지 않다면 페이지 할당 반환, 파일 정보 해제 후 false 리턴
			free (li);
			return false;
		}
	}
	memset (page->va+li->page_read_bytes, 0, li->page_zero_bytes);									// 문제 없다면 memset 진행 (dst, value, size)
	file_close (li -> file);																		// memset 후 파일 닫기 및 파일 정보 해제
	free (li);
	return true;
}

/* Project 3. AP : VM을 위한 load segment 함수 구현 */
/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {				// 인자 (파일, 위치, 페이지, 읽을 바이트 수, UPAGE+READ_BYTE, 쓰기 여부)
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);							
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	off_t read_ofs = ofs; 														// 읽는 위치에 대한 정보
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		struct load_info *aux = malloc(sizeof(struct load_info));				// 로드 되는 파일에 대한 정보 및 읽기 위해 할당
		aux->file = file_reopen(file);											// file 다시 오픈
		aux->ofs = read_ofs;													// 읽는 위치
		aux->page_read_bytes = page_read_bytes;									// 읽어야 할 바이트 수
		aux->page_zero_bytes = page_zero_bytes;

		if (!vm_alloc_page_with_initializer (VM_ANON, upage,					// 초기화 실패 했다면 aux free하고 false 리턴
					writable, lazy_load_segment, aux)) {
			free (aux);
			return false;
		}
			
		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
		read_ofs += PGSIZE;
	}
	return true;
}

/* Project 3. AP : lazy_load에 맞는 스택 셋업 필요 */
/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	
	if (!vm_alloc_page (VM_ANON | VM_MARKER_0, stack_bottom ,true)) return false;		// page 할당 받기
	
	success = vm_claim_page(stack_bottom);											// page claim 하기
	if (success)																	// 성공 시 스택 포인터 업데이트
		if_->rsp = USER_STACK;
	else
		return false;

	return success;
}
#endif /* VM */
