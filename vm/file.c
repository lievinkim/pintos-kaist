/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"

/* Project 3. MMF에 따른 헤더 추가 */
#include "threads/vaddr.h"
#include "vm/file.h"
#include <string.h>
#include "threads/malloc.h"
#include "threads/mmu.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* Project 3. MMF : mmap 기록을 mmap_file_info에 저장 후 리스트로 관리 */
static struct list mmap_file_list;

struct mmap_file_info{
	struct list_elem elem;
	uint64_t start;				// start addr of final page
	uint64_t end;
};

/* Project 3. MMF : mmap_file_info 리스트 초기화 */
/* The initializer of file vm */
void
vm_file_init (void) {
	list_init (&mmap_file_list);
}

/* Project 3. MMF : vm_alloc_page_with_initializer에서 fetch 할 file_backed_initializer 함수 구현 */
/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {

	/* Set up the handler */
	struct file* file = ((struct mmap_info*)page ->uninit.aux)->file;		// mmap 할 파일 정보 가져오기
	page->operations = &file_ops;											// 파일 백업 페이지의 operations 정보 삽입하기

	struct file_page *file_page = &page->file;								// 파일 페이지 정보 가져오기
	file_page -> file = file;												// 파일 페이지 정보 업데이트
	
	return true;
}

/* Project 3. Swap In/Out : 파일 백업 페이지를 위한 스왑 인 함수 구현 */
/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page = &page->file;								// 페이지의 파일 정보 가져오기
	if (file_page->file == NULL) return false;								// 파일 정보 없으면 false 리턴

	file_seek (file_page->file, file_page->ofs);							// 파일 위치 찾기
	off_t read_size = file_read (file_page->file, kva, file_page->size);	// 읽어야 할 사이즈 가져오기
	if (read_size != file_page->size) return false;							// 사이즈 검증
	if (read_size < PGSIZE)													// PG 사이즈 보다 작을 경우에만 memset 진행
		memset (kva + read_size, 0, PGSIZE - read_size);					// 남은 부분은 0으로 세팅

	return true;
}

/* Project 3. Swap In/Out : 파일 백업 페이지를 위한 스왑 아웃 함수 구현 */
/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page = &page->file;								// 페이지의 파일 정보 가져오기
	struct thread *curr = thread_current ();								// 현재 진행 중이 스레드의 정보 가져오기

	if (pml4_is_dirty (curr->pml4, page->va)) {								// 해당 페이지의 더티 여부 체크
		file_seek (file_page->file, file_page->ofs);						// 파일과 시작 위치로 탐색
		file_write (file_page->file, page->va, file_page->size);			// 쓰기 진행 (스왑 아웃)
		pml4_set_dirty (curr->pml4, page->va, false);						// 더티 상태 초기화
	}

	// Set "not present" to page, and clear.
	pml4_clear_page (curr->pml4, page->va);									// pm14에서 페이지 삭제
	page->frame = NULL;														// 물리 메모리 해제에 따른 NULL 값 대입

	return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
		// TODO: On mmap_exit sometimes empty file content
	struct file_page *file_page = &page->file;
	//if dirty, write back to file
	if (pml4_is_dirty (thread_current() -> pml4, page -> va)){
		file_seek (file_page->file, file_page->ofs);
		file_write (file_page->file, page->va, file_page->size);
	}
	file_close (file_page->file);

	if (page->frame != NULL) {
		list_remove (&page->frame->elem);
		free (page->frame);
	}
}


/* Project 3. MMF : mmap을 위한 lazy load 구현 */
static bool
lazy_load_file (struct page* page, void* aux){
	struct mmap_info* mi = (struct mmap_info*) aux;							// mmap 할 정보 가져오기

	file_seek (mi->file, mi->offset);										// 파일의 current 포지션은 offset 포지션으로 변경
	page->file.size = file_read (mi->file, page->va, mi->read_bytes);		// 페이지 정보 업데이트 (file_read을 통해 읽은 크기)
	page->file.ofs = mi->offset;											// 페이지 정보 업데이트 (오프셋)

	if (page->file.size != PGSIZE){											// page-aligned 되어 있어야 함
		memset (page->va+page->file.size, 0, PGSIZE-page->file.size);		// page-aligned 안되어 있으면 나머지는 0으로 세팅
	}

	pml4_set_dirty(thread_current()->pml4, page->va, false);				// dirty를 false 상태로 세팅
	free(mi);																// mi 해제

	return true;
}

/* Project 3. MMF : mmap 시스템 콜에서 호출 */
/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
	
	off_t ofs;															// 오프셋 정보
	uint64_t read_bytes;												// 읽어야 할 바이트 수 정보

	for (uint64_t i = 0; i < length; i += PGSIZE){						// 길이를 PGSIZE 단위로 나누어 작업 시작 (page-aligned)

		struct mmap_info* mi = malloc (sizeof (struct mmap_info));		// mmap 할 정보 가져오기

		ofs = offset + i;												// offset에서 page 단위 만큼 더함 (i는 pgsize의 배수)
		read_bytes = length - i >= PGSIZE ? PGSIZE : length - i;		// PGSIZE 기준으로 자르기 (길면 PGSIZE, 짧으면 length 만큼)

		mi->file = file_reopen (file);									// 개별적이고 독립적인 참조를 위해 reopen 함수 사용
		mi->offset = ofs;												// 변경된 오프셋 정보 반영
		mi->read_bytes = read_bytes;									// 변경된 읽어야 할 바이트 수 정보 반영

		vm_alloc_page_with_initializer (VM_FILE, (void*) ((uint64_t) addr + i), writable, lazy_load_file, (void*) mi);	// lazy_load_file 기반으로 초기화 진행
	}

	struct mmap_file_info* mfi = malloc (sizeof (struct mmap_file_info));	// mmap 파일 정보 저장을 위한 메모리 할당
	mfi->start = (uint64_t) addr;											// start 정보 입력
	mfi->end = (uint64_t) pg_round_down((uint64_t) addr + length -1);		// end 정보 입력
	
	list_push_back(&mmap_file_list, &mfi->elem);							// mmap 파일 리스트에 추가

	return addr;

}

/* Project 3. MMF : munmap 시스템 콜에서 호출 */
/* Do the munmap */
void
do_munmap (void *addr) {

	//traverse mmap_file_list and find appropriate set, and destroy all
	if (list_empty (&mmap_file_list)) return;		// mmap 파일 리스트가 비어 있다면 munmap 할게 없기 때문에 단순 리턴

	for (struct list_elem* i = list_front (&mmap_file_list); i != list_end (&mmap_file_list); i = list_next (i))
	{
		struct mmap_file_info* mfi = list_entry (i, struct mmap_file_info, elem);			// 리스트 돌며 mmap 파일 정보 가져오기

		if (mfi -> start == (uint64_t) addr){												// 만약 start가 addr와 같다면? 해제 시작

			/* 1) SPT에서 해당되는 페이지 찾은 후 삭제 */
			for (uint64_t j = (uint64_t)addr; j<= mfi->end; j += PGSIZE){					// end 만큼 PGSIZE 단위로 돌기
				struct page* page = spt_find_page(&thread_current()->spt, (void *)j);		// spt 검색해서 page 정보 가져오기
				spt_remove_page(&thread_current()->spt, page);								// remove 진행 (SPT에서 페이지 삭제)
			}

			list_remove(&mfi->elem);														// 2) mmap 파일 관리 목록에서 삭제
			free(mfi);																		// 3) mmap 파일 관리 정보 해제
			return;
		}
	}

}
