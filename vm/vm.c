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

/* Project 3. MM : frame_list 선언 */
static struct list frame_list;

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

	/* Project 3. MM : frame_list 초기화 */
	list_init (&frame_list);

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

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */

		/* TODO: Insert the page into the spt. */
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
	vm_dealloc_page (page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */

	return NULL;
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

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);

	ASSERT (frame->kva != NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr UNUSED) {
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
	struct page *page = NULL;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */

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

	list_push_back (&frame_list, &frame->elem);						// frame 리스트에 추가


	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	/* page의 virtual address를 frame의 physical address로 맵핑하기 위해 page table entry 삽입 */
	/* supplemental page table - page table - physical address 에서 가운데 page table에 올리는 작업*/
	if (!pml4_set_page (curr -> pml4, page -> va, frame->kva, page -> writable))
		return false;


	return swap_in (page, frame->kva);
}

/* Project 3. MM : 해싱 함수 구현 */
static uint64_t
page_hash (const struct hash_elem *p_, void *aux UNUSED) {
	const struct page *p = hash_entry(p_, struct page, hash_elem);
	return hash_bytes(&p->va, sizeof p->va);
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

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
}
