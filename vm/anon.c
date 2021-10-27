/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"

/* Project 3. AP : Page Cleanup 작업을 위한 헤더 추가 */
#include "threads/malloc.h" 

/* Project 3. Swap In/Out : 어나니머스 페이지를 위한 스왑 디스크 생성에 필요한 헤더 추가 */
#include "threads/vaddr.h"
#include <bitmap.h>

/* Project 3. Swap In/Out : 스왑 아웃에 필요한 헤더 추가 */
#include "threads/mmu.h"

/* Project 3. Swap In/Out : 어나니머스 페이지를 위한 스왑 디스크 생성에 필요한 값 정의 */
/*
 * 정리하자면 DISK_SECTOR_SIZE는 말 그대로 한 섹터의 사이즈
 * SECTORS_PER_PAGE는 페이지를 담기 위해 필요한 섹터의 수
 * CEILING 로직을 활용하여 도출 한 것
 */
#define CEILING(x, y) (((x) + (y) - 1) / (y))
#define SECTORS_PER_PAGE CEILING(PGSIZE, DISK_SECTOR_SIZE)

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

/* Project 3. AP : anonymous page initializer 구현 */
static const struct page_operations anon_stack_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON | VM_MARKER_0,
};

/* Project 3. Swap In/Out : 스왑 테이블 생성에 필요한 구조체 추가 */
static struct bitmap *swap_table;

/* Project 3. Swap In/Out : 어나니머스 페이지를 위해 스왑 디스크 생성 */
/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {

	/* TODO: Set up the swap_disk. */
	swap_disk = disk_get(1, 1);								// 정해진 룰에 따라 1, 1을 인자로 넘김

	disk_sector_t num_sector = disk_size(swap_disk);		// swap_disk의 사이즈 가져오기
	size_t max_slot = num_sector / SECTORS_PER_PAGE;		// 전체 사이즈에서 페이지에 필요한 사이즈를 나누어 최대 슬롯 수 저장

	swap_table = bitmap_create(max_slot);					// max_slot 기반으로 swap_table 생성

}

/* Project 3. AP : anonymous page initializer 구현 */
/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {	// 인자 (페이지, 타입, kva)

	/* Set up the handler */
	page->operations = &anon_ops;								// 페이지 operations에 anon_ops 할당
	if (type & VM_MARKER_0) page->operations = &anon_stack_ops;
	struct anon_page *anon_page = &page->anon;					// 페이지의 anon을 anon_page 구조체에 할당
	anon_page->owner = thread_current();						// 현재 실행 중인 스레드를 anon_page 오너로 설정

	/* Project 3. Swap In/Out : 페이지 별 스왑 슬롯 idx 설정 */
	anon_page->swap_slot_idx = INVALID_SLOT_IDX;				// 스왑 테이블 저장 시 idx 값 저장

	return true;
}

/* Project 3. Swap In/Out : 스왑 인 진행 */
/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {

	struct anon_page *anon_page = &page->anon;											// 페이지의 어나니머스 정보 가져오기
	if (anon_page->swap_slot_idx == INVALID_SLOT_IDX) return false;						// 할당 받은 슬롯 IDX가 없다면 스왑 아웃 상태 아님으로 false 리턴

	disk_sector_t sec_no;																// 디스크 섹터 넘버 변수
	// Read page from disk with sector size chunk
	for (int i = 0; i < SECTORS_PER_PAGE; i++) {										// 한 페이지당 할당되는 섹터의 수

		// convert swap slot index to reading sector number
		sec_no = (disk_sector_t) (anon_page->swap_slot_idx * SECTORS_PER_PAGE) + i;		// idx에 섹터 수를 곱하면 위치 반환. 여기서부터 i를 늘리며 읽어오기
		off_t ofs = i * DISK_SECTOR_SIZE;												// ofs은  DISK_SECTOR_SIZE 만큼 증가 (512, 1024, ... )
		disk_read (swap_disk, sec_no, kva+ofs);											// 위 정보를 기반으로 디스크 읽기 진행 (스왑 인)
	}

	// Clear swap table
	bitmap_set (swap_table, anon_page->swap_slot_idx, false);							// 스왑 인 후 스왑 테이블에서 off 상태로 전환
	anon_page->swap_slot_idx = INVALID_SLOT_IDX;										// 스왑 인 후 슬롯 IDX 초기화

}

/* Project 3. Swap In/Out : 스왑 아웃 진행 */
/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;

	// Get swap slot index from swap table
	size_t swap_slot_idx = bitmap_scan_and_flip (swap_table, 0, 1, false);				// 스왑 테이블에서 스왑 슬롯 인덱스 값 가져오기
	if (swap_slot_idx == BITMAP_ERROR)													// 가용 스왑 슬롯이 없는 경우
		PANIC("There is no free swap slot!");											// 패닉 발생

	// Copy page frame content to swap_slot
	if (page == NULL || page->frame == NULL || page->frame->kva == NULL)				// 스왑 아웃 할 페이지의 유효성 검증
		return false;

	disk_sector_t sec_no;																// 디스크 섹터 넘버 변수
	// Write page to disk with sector size chunk
	for (int i = 0; i < SECTORS_PER_PAGE; i++) {										// 한 페이지당 할당되는 섹터의 수
		// convert swap slot index to writing sector number
		sec_no = (disk_sector_t) (swap_slot_idx * SECTORS_PER_PAGE) + i;				// idx에 섹터 수를 곱하면 위치 반환. 여기서부터 i를 늘리며 읽어오기
		off_t ofs = i * DISK_SECTOR_SIZE;												// ofs은  DISK_SECTOR_SIZE 만큼 증가 (512, 1024, ... )
		disk_write (swap_disk, sec_no, page->frame->kva + ofs);							// 위 정보를 기반으로 디스크 쓰기 진행 (스왑 아웃)
	}

	anon_page->swap_slot_idx = swap_slot_idx;											// 스왑 아웃 후 슬롯 IDX 업데이트

	// Set "not present" to page, and clear.
	pml4_clear_page (anon_page->owner->pml4, page->va);									// PML4에서 페이지 삭제
	pml4_set_dirty (anon_page->owner->pml4, page->va, false);							// PML4에서 dirty 상태 초기화
	page->frame = NULL;																	// 물리 메모리 해제에 따른 초기화

	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {

	/* Project 3. AP : Page Cleanup 작업을 위한 코드 */
	if (page -> frame!= NULL){
		list_remove (&page->frame->elem);
		free(page->frame);
	}
	
	/* Project 3. Swap In/Out : 삭제하려는 어나니머스 페이지가 swapped 된 케이스인 경우 */
	else {
		struct anon_page *anon_page = &page->anon;
		
		ASSERT (anon_page->swap_slot_idx != INVALID_SLOT_IDX);							// 스왑 된 케이스가 아니면 안됨
		bitmap_set (swap_table, anon_page->swap_slot_idx, false);						// 스왑 테이블 초기화
	}
}
