/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"

/* Project 3. MM : 해시테이블 사용하기 위해 헤더 추가 */
#include "lib/kernel/hash.h"

/* Project 3. MM : Virtual Address 위해 헤더 추가 */
#include "threads/vaddr.h"

/* Project 3. MM : page claim을 위해 mmu 헤더 추가 */
#include "threads/mmu.h"

/* Project 3. AP : SPT-REVISIT 작업 진행 */
#include <string.h>

/* Project 3. MM : frame_list 선언 */
static struct list frame_list;

/* Project 3. AP : SPT-REVIST KILL을 위한 lock 설정 */
static struct lock spt_kill_lock;

/* Project 3. Swap In/Out : clock 알고리즘을 위한 lock 구조체 선언 */
static struct lock clock_lock;

/* Project 3. Swap In/Out : clock 알고리즘에 따른 대상 정보 */
static struct list_elem *clock_elem;

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */

	/* Project 3. AP : SPT-REVIST KILL을 위한 lock 설정 */
	lock_init(&spt_kill_lock);

	/* Project 3. MM : frame_list 초기화 */
	list_init (&frame_list);

	/* Project 3. Swap In/Out : clock 알고리즘을 위한 lock init */
	lock_init (&clock_lock);

	/* Project 3. Swap In/Out : 처음에는 당연히 NULL 값 */
	clock_elem = NULL;
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

/* Project 3. MM : page_hash, page_less 함수 선언 */
static uint64_t page_hash (const struct hash_elem *p_, void *aux UNUSED);
static bool page_less (const struct hash_elem *a_,
           const struct hash_elem *b_, void *aux UNUSED);

/* Project 3. Swap In/Out : clock 알고리즘에 따라 list를 원형 테이블로 바꾸기 위한 함수 선언 */
static struct list_elem *list_next_cycle (struct list *lst, struct list_elem *elem);

/* Project 3. AP : 최초 페이지 할당 후 initializer fetch 작업해 주는 함수 */
/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {								// 인자 (type, va, writable, initializer, aux)

	struct supplemental_page_table *spt = &thread_current ()->spt; 		// 현재 실행 중인 스레드의 SPT 정보 가져오기
	bool writable_aux = writable;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {							// 전달 받은 va가 spt에 없는 경우에 진행 (처음 생성한 페이지라는 뜻)
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */

		ASSERT (VM_TYPE(type) != VM_UNINIT)								// 모든 페이지는 처음에 VM_UNINIT으로 생성된다.

		struct page *page = malloc(sizeof(struct page));				// 페이지 구조체 malloc 할당

		if (VM_TYPE(type) == VM_ANON) {
			uninit_new(page, upage, init, type, aux, anon_initializer);	// initializer fetch (구조체 초기화 작업 진행)
		} else if (VM_TYPE(type) == VM_FILE) {

			/* Project 3. MMF : 파일 백업 페이지를 위한 initializer fetch */
			uninit_new (page, upage, init, type, aux, file_backed_initializer);
		}
		
		page->writable = writable_aux;										// 전달 받은 쓰기 가능 정보 저장하기

		/* TODO: Insert the page into the spt. */
		spt_insert_page(spt, page);										// spt에 page 삽입하기
		return true;													// 모든 작업 완료 시 true 반환
	}
err:
	return false;
}

/* Project 3. MM : spt에서 VA 찾고 페이지 리턴하는 함수 구현 */
/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
	/* TODO: Fill this function. */
	struct page page;

	page.va = pg_round_down(va);											// 가장 가까운 페이지 영역으로 내림
	struct hash_elem *e = hash_find(spt->page_table, &page.hash_elem);		// 해시 검색 (spt->page_table에서 page.hash_elem을 가진 값을 찾아서 리턴)	

	if (e == NULL) return NULL;												// 검색 결과 없으면 NULL 리턴

	struct page* result = hash_entry(e, struct page, hash_elem);			// 검색 결과로 찾은 해시 정보를 기반으로 page 정보 가져오기
	ASSERT((va < result->va + PGSIZE) && va >= result->va);					// PGSIZE 크기를 가져야 함을 체크

	return result;
}

/* Project 3. MM : Page를 spt에 삽입하는 함수 구현 */
/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED,
		struct page *page UNUSED) {
	/* TODO: Fill this function. */
	struct hash_elem *result = hash_insert(spt->page_table, &page->hash_elem);	// page를 해시 테이블에 삽입하기

	return (result == NULL) ? true : false;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	/* Project 3. MMF : munmap 시 사용 */
	struct hash_elem* e = hash_delete (spt -> page_table, &page ->hash_elem);  	// hash 테이블로 관리하기 때문에 hash 테이블에서 가져오기
	if (e != NULL) vm_dealloc_page (page);										// 해시 테이블에 값이 있다면 해당 값 dealloc 진행
	return true;
}

/* Project 3. Swap In/Out : clock 알고리즘에 따라 list를 원형 테이블로 바꾸기 위한 함수 구현 */
static struct list_elem *
list_next_cycle (struct list *lst, struct list_elem *elem) {
	struct list_elem *cand_elem = elem;							// elem의 list_elem 정보 가져오기

	if (cand_elem == list_back (lst))								// 만약 후보 elem이 리스트의 마지막인 경우,
		cand_elem = list_front (lst);								// 리스트 앞에서 다시 시작
	else
		cand_elem = list_next (cand_elem);							// 그렇지 않은 경우에는 다음 친구 데려오기

	return cand_elem;
}

/* Project 3. Swap In/Out : clock 알고리즘에 따른 victim 구하는 함수 구현 */
/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {

	struct frame *victim = NULL;
	/* TODO: The policy for eviction is up to you. */
	/* Clock Algorithm 선택 - https://kouzie.github.io/operatingsystem/%EA%B0%80%EC%83%81%EB%A9%94%EB%AA%A8%EB%A6%AC/#clock-algorithm */

	struct thread *curr = thread_current ();								// 현재 실행 중인 스레드 정보 가져오기

	lock_acquire (&clock_lock);												// 스레드 간 발생할 수 있는 동기화 및 레이스 이슈 방지

	struct list_elem *vict_elem = clock_elem;								// clock elem 정보 가져오기 (최초에는 NULL인 것임)

	if (vict_elem == NULL && !list_empty (&frame_list))						// vict 정보가 없는데 (최초), frame list가 비어있지 않다면
		vict_elem = list_front (&frame_list);								// list의 첫 번째를 vict_elem으로 가져오기 (해당 elem 기반으로 탐색 시작)

	while (vict_elem != NULL) {												// vict 정보가 있다면,

		// Check frame accessed
		victim = list_entry (vict_elem, struct frame, elem);				// frame 리스트에 있는 victim 후보 하나씩 조회하기

		if (!pml4_is_accessed (curr->pml4, victim->page->va))				// 최근에 접근한 적이 없는 페이지다?
			break; // Found!												// 당첨
		
		pml4_set_accessed (curr->pml4, victim->page->va, false);			// 한번 체크한 친구는 지나갈 때 0으로 다시 바꿔줌
		vict_elem = list_next_cycle (&frame_list, vict_elem);				// frame_list의 다음 친구를 vict_elem으로 설정
	}			
	
	clock_elem = list_next_cycle (&frame_list, vict_elem);					// break 시 선택 된 victim은 빠지기 때문에 그 다음 친구를 clock_elem으로 선정 (Tick Clock)
	list_remove (vict_elem);												// victim은 리스트에서 삭제
	
	lock_release (&clock_lock);												// 락 해제

	return victim;

}

/* Project 3. Swap In/Out : 구한 victim 축출하는 함수 구현 */
/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();							// victim 선정

	/* TODO: swap out the victim and return the evicted frame. */
	if (victim == NULL) return NULL;										// victim이 선정되지 않았다면 NULL 리턴

	/* Swap out the victim and return the evicted frame. */
	struct page *page = victim->page;										// victim의 페이지 구조체 가져오기
	bool swap_done = swap_out (page);										// victim의 페이지 스왑 아웃 시키기

	if (!swap_done) PANIC("Swap is full!\n");								// swap이 안되었다면 꽉 찼다는 뜻임으로 PANIC 발생

	victim->page = NULL;													// victim의 페이지 초기화
	memset (victim->kva, 0, PGSIZE);										// 해당 페이지의 값 0으로 초기화

	return victim;															// 축출 완료 및 해당 victim 전달
}

/* Project 3. MM : frame 얻기 위한 함수 구현 */
/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	/* TODO: Fill this function. */
	struct frame *frame = malloc(sizeof(struct frame));
	frame->kva = palloc_get_page(PAL_USER);
	frame->page = NULL;

	/* Project 3. Swap In/Out : frame이 꽉 찼을 때 Swap In/Out 진행 */
	// ASSERT (frame != NULL);
	// ASSERT (frame->page == NULL);

	if (frame->kva == NULL) {
	  free(frame);						// 기존에 할당 받은 frame은 사용할 수 없음으로 우선 해제
	  frame = vm_evict_frame();			// 해제 후 축출한 frame(victim) 정보 가져오기
	}

	ASSERT (frame->kva != NULL);
	return frame;
}

/* Project 3. SG : Stack Growth 함수 구현 */
/* vm_try_handle_fault에서 전달 받은 주소 기반으로 새로운 페이지 추가 */
/* Growing the stack. */
static void
vm_stack_growth (void *addr UNUSED) {
	void *stack_bottom = pg_round_down (addr);									// 늘려을 때의 스택 바닥 주소
	size_t req_stack_size = USER_STACK - (uintptr_t)stack_bottom;				// 늘렸을 때의 스택 영역 사이즈

	if (req_stack_size > (1 << 20)) PANIC("Stack limit exceeded!\n");			// 해당 프로젝트에서는 스택의 영역을 1MB로 제한

	void *growing_stack_bottom = stack_bottom;									// 늘려주기 위한 보조 장치
	
	while ((uintptr_t) growing_stack_bottom < USER_STACK &&						// 늘렸을 때의 스택 바닥 주소가 USER_STACK 보단 작아야 함
		vm_alloc_page (VM_ANON | VM_MARKER_0, growing_stack_bottom, true)) {	// vm_alloc_page를 통해 할당 받아야 함

		/* 할당이 완료되면 growing_stack_bottom의 높이를 한 페이지 만큼 올려줌 */
		/* 이는 여러 페이지를 늘렸을 때 최하단부터 하나씩 늘리기 위함 - 최대 늘렸을 때의 유효성을 체크하기 위함 인듯 */
		/* 그러나 애초에 vm_try_handle_fault에서 최대 1페이지 크기만큼만 전달 받기 때문에 사실 없어도 될 듯? */
		growing_stack_bottom += PGSIZE;
	};

	vm_claim_page(stack_bottom);												// 요청한 페이지에 대해서 Lazy laod
}

/* Project 3. AP : 기본적인 핸들링 내용 추가 */
/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
	return false;
}

/* Project 3. AP : 폴트 발생한 주소에 상응하는 page 구조체 찾고 해결하는 함수 구현 */
/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {

	struct thread *curr = thread_current();
	struct supplemental_page_table *spt UNUSED = &curr->spt;

	/* TODO: Validate the fault */
	/* TODO: Your code goes here */
	if (is_kernel_vaddr(addr) && user) return false;							// 해당 주소가 커널 영역이면 false 리턴


	/* Project 3. SG : Stack Growth를 위해 저장된 스택 포인터 가져오기 */
	void *stack_bottom = pg_round_down(curr->saving_rsp);

	/* Project 3. SG : 스택 포인터 유효성 검사 진행 후 vm_stack_growth 함수 호출 */
	/* 
	 * 조건 1.
	 * 1) 쓰기 가능 여부
	 * 2) addr이 stack_bottom에서 1 페이지 이내에 존재 하는지 여부
	 * 3) addr가 USER_STACK 보다 밑에 있는 지 여부
	 *
	 */
	if (write && (stack_bottom - PGSIZE <= addr && (uintptr_t) addr < USER_STACK)) {
	  /* Allow stack growth writing below single PGSIZE range
	   * of current stack bottom inferred from stack pointer. */
	  vm_stack_growth (addr);
	  return true;
	}


	struct page* page = spt_find_page (spt, addr);
	if (page == NULL) return false;												// 페이지를 못 찾았을 경우 false 리턴
	if (write && !not_present) return vm_handle_wp(page);						// write_protected page인 경우 핸들링

	return vm_do_claim_page (page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Project 3. MM : page claim을 위한 함수 구현 */
/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va UNUSED) {
	/* TODO: Fill this function */
	struct page *page = spt_find_page(&thread_current()->spt, va);	// VM define 시 spt 멤버 노출
	
	if (page == NULL) return false;									// 만약 찾기 실패 시 false 리턴

	return vm_do_claim_page (page);									// 찾기 성공 시 do claim 함수 호출
}

/* Project 3. MM : page 클레임에 따라 spt에서 찾은 page를 실제로 옮기는 함수 구현 */
/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();							// 프레임 할당 받기
	struct thread *curr = thread_current ();						// 실행 중인 스레드 정보 받기

	ASSERT (page != NULL);											// page valid check
	ASSERT (frame != NULL);											// frame valid check

	/* Set links */
	frame->page = page;												// frame의 page에 page 할당
	page->frame = frame;											// page의 frame에 frame 할당

	/* Project 3. Swap In/Out : Clock 알고리즘에 따라 clock_elem 확인 후 frame_list에 넣을 위치 정함 */
	if (clock_elem != NULL)
		list_insert (clock_elem, &frame->elem);						// clock_elem 존재 시 그 전에 위치 시킴
	else
		list_push_back (&frame_list, &frame->elem);					// 없으면 기존과 동일하게 frame 리스트에 추가

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	/* page의 virtual address를 frame의 physical address로 맵핑하기 위해 page table entry 삽입 */
	/* supplemental page table - page table - physical address 에서 가운데 page table에 올리는 작업*/
	if (!pml4_set_page (curr->pml4, page->va, frame->kva, page->writable))
		return false;
	return swap_in (page, frame->kva);
}

/* Project 3. MM : 해싱 함수 구현 */
static uint64_t
page_hash (const struct hash_elem *p_, void *aux UNUSED) {
  const struct page *p = hash_entry (p_, struct page, hash_elem);
  return hash_bytes (&p->va, sizeof p->va);
}

/* Project 3. MM : 해시값 비교 함수 구현 */
static bool
page_less (const struct hash_elem *a_,
           const struct hash_elem *b_, void *aux UNUSED) {
  const struct page *a = hash_entry (a_, struct page, hash_elem);
  const struct page *b = hash_entry (b_, struct page, hash_elem);

  return a->va < b->va;		// a가 b보다 낮으면 true, 높으면 false 리턴
}

/* Project 3. MM : spt_init 함수 구현 */
/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	struct hash* page_table = malloc(sizeof(struct hash));		// page_table을 위한 메모리 할당 (커널 영역에 저장)
	hash_init(page_table, page_hash, page_less, NULL);			// page_table 초기화
	spt->page_table = page_table;								// spt의 page_table에 저장
}

/* Project 3. AP : SPT-REVISIT 작업 진행 */
/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
	
	struct hash_iterator i;
	hash_first(&i, src->page_table);															// spt 내 모든 페이지를 돌기 위한 세팅
	while (hash_next(&i)) {
		struct page *page = hash_entry(hash_cur(&i), struct page, hash_elem);					// current hash에 대한 페이지 정보 가져오기

		/* Handle UNINIT pages*/
		if(page->operations->type == VM_UNINIT) {												// 해당 페이지가 UNINIT 페이지인 경우
			vm_initializer *init = page->uninit.init;											// UNINIT 내 세팅해 놓은 initializer 가져오기
			bool writable = page->writable;
			int type = page->uninit.type;
			if (type & VM_ANON) {																// 기존 세팅 값이 ANON인 경우
				struct load_info* li = malloc (sizeof (struct load_info));						// uninit의 initialize 진행
				li->file = file_duplicate (((struct load_info *)page->uninit.aux)->file);
				li->page_read_bytes = ((struct load_info *)page->uninit.aux)->page_read_bytes;
				li->page_zero_bytes = ((struct load_info *)page->uninit.aux)->page_zero_bytes;
				li->ofs = ((struct load_info *) page->uninit.aux)->ofs;
				vm_alloc_page_with_initializer (type, page->va, writable, init, (void*)li);				
			} else if (type & VM_FILE) {														// 기존 세팅 값이 FILE인 경우 (아무것도 안함)
				// Do nothing (should not inherit)
			}
		/* Handle ANON pages */
		} else if (page_get_type(page) == VM_ANON){												// 해당 페이지가 ANON 페이지인 경우

			if (!vm_alloc_page (page->operations->type, page->va, page->writable))				// 페이지 할당
				return false;

			struct page* new_page = spt_find_page (&thread_current()->spt, page->va);

			if (!vm_do_claim_page (new_page))													// 바로 claim
				return false;

			memcpy (new_page->frame->kva, page->frame->kva, PGSIZE);							// 복사 진행
		} else if (page_get_type(page) == VM_FILE){												// 해당 페이지가 FILE 페이지인 경우 (아무것도 안함)
			// Do nothing (should not inherit)
		}

	}

	return true;

}

/* Project 3. AP : SPT-REVISIT 작업 진행 */
static void
spt_destroy (struct hash_elem *e, void *aux UNUSED){
	struct page *page = hash_entry (e, struct page, hash_elem);					// 해시 엔트리로 페이지 가져오기
	ASSERT (page != NULL);														// PAGE가 이미 NULL이면 ASSERT
	destroy (page);																// PAGE 삭제
	free (page);																// PAGE 해제
}

/* Project 3. AP : SPT-REVISIT 작업 진행 */
/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */

	if (spt->page_table == NULL) return;										// 이미 NULL이라면 작업 필요 없음
	lock_acquire(&spt_kill_lock);												// 작업 전 lock 획득
	hash_destroy(spt->page_table, spt_destroy);									// page_table 돌아다니며 spt_destroy 진행
	free(spt->page_table);														// 진행 후 page_table 해제
	lock_release(&spt_kill_lock);												// 작업 전 lock 반환
}