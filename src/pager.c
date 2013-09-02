#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <elkvm.h>
#include <pager.h>
#include <vcpu.h>

int kvm_pager_initialize(struct kvm_vm *vm, int mode) {
	if(vm->fd < 1) {
		return -EIO;
	}

	struct elkvm_memory_region *pts_region = elkvm_region_create(vm, 0x400000);
	if(pts_region == NULL) {
		return -ENOMEM;
	}

	vm->pager.mode = mode;
	vm->pager.other_chunks = NULL;

	vm->pager.host_pml4_p = pts_region->host_base_p;
	uint64_t pml4_guest_physical = host_to_guest_physical(&vm->pager,
			pts_region->host_base_p);
	vm->pager.guest_next_free = KERNEL_SPACE_BOTTOM;

	int err = kvm_pager_create_page_tables(&vm->pager, mode);
	if(err) {
		return err;
	}

	err = kvm_vcpu_set_cr3(vm->vcpus->vcpu, pml4_guest_physical);
	if(err) {
		return err;
	}

	return 0;
}

struct kvm_userspace_memory_region *kvm_pager_alloc_chunk(struct kvm_pager *pager,
    void *addr, uint64_t chunk_size, int flags) {
  struct kvm_userspace_memory_region *chunk;
  chunk = malloc(sizeof(struct kvm_userspace_memory_region));
  if(chunk == NULL) {
    return NULL;
  }

	chunk->userspace_addr = (__u64)addr;
  chunk->guest_phys_addr = pager->total_memsz;
	chunk->memory_size = chunk_size;
	chunk->flags = flags;
  pager->total_memsz = pager->total_memsz + chunk_size;

	int chunk_count = kvm_pager_append_mem_chunk(pager, chunk);
	if(chunk_count < 0) {
    free(chunk);
    return NULL;
	}
	/* system chunk has slot 0, so we need to add 1 to all user chunks */
	chunk->slot = chunk_count + 1;

  return chunk;
}

int kvm_pager_create_mem_chunk(struct kvm_pager *pager, void **chunk_host_p,
    int chunk_size) {

	if(pager == NULL) {
    *chunk_host_p = NULL;
		return -EIO;
	}

	/* keep sizes page aligned */
	if((chunk_size & ~0xFFF) != chunk_size) {
		return -EIO;
	}

	int err = posix_memalign(chunk_host_p, 0x1000, chunk_size);
	if(err) {
		return err;
	}
  struct kvm_userspace_memory_region *chunk =
    kvm_pager_alloc_chunk(pager, chunk_host_p, chunk_size, 0);
  if(chunk == NULL) {
    free(*chunk_host_p);
    *chunk_host_p = NULL;
    return -ENOMEM;
  }

	return 0;
}

int kvm_pager_append_mem_chunk(struct kvm_pager *pager,
		struct kvm_userspace_memory_region *chunk) {

	if(pager->other_chunks == NULL) {
		pager->other_chunks = malloc(sizeof(struct chunk_list));
		if(pager->other_chunks == NULL) {
			return -ENOMEM;
		}
		pager->other_chunks->chunk = chunk;
		pager->other_chunks->next = NULL;
		return 0;
	}

	int chunk_count = 0;
	struct chunk_list *current = pager->other_chunks;
	while(current->next != NULL) {
		chunk_count++;
		current = current->next;
	}

	current->next = malloc(sizeof(struct chunk_list));
	if(current->next == NULL) {
		return -ENOMEM;
	}

	chunk_count++;
	current = current->next;
	current->chunk = chunk;
	current->next = NULL;

	return chunk_count;
}

int kvm_pager_create_page_tables(struct kvm_pager *pager, int mode) {
	if(pager == NULL ||  pager->host_pml4_p == NULL) {
		return -EINVAL;
	}
	if(mode != PAGER_MODE_X86_64) {
		return -ENOSYS;
	}

	if(pager->system_chunk.memory_size < 0x400000) {
		return -1;
	}

	memset(pager->host_pml4_p, 0, 0x400000);
	pager->host_next_free_tbl_p = pager->host_pml4_p + 0x1000;

	return 0;
}

int kvm_pager_is_invalid_guest_base(struct kvm_pager *pager, uint64_t guest_base) {

	/* keep base addresses page aligned */
	if((guest_base & ~0xFFF) != guest_base) {
		return 1;
	}

	if(pager->system_chunk.guest_phys_addr <= guest_base &&
			guest_base < pager->system_chunk.guest_phys_addr +
				pager->system_chunk.memory_size) {
		return 1;
	}

	struct chunk_list *cl = pager->other_chunks;
	while(cl != NULL) {
		if(cl->chunk->guest_phys_addr <= guest_base &&
				guest_base < cl->chunk->guest_phys_addr + cl->chunk->memory_size) {
			return 1;
		}
		cl = cl->next;
	}

	return 0;
}

struct kvm_userspace_memory_region *
	kvm_pager_find_region_for_host_p(struct kvm_pager *pager, void *host_mem_p) {
		if(((void *)pager->system_chunk.userspace_addr <= host_mem_p) &&
				(host_mem_p < ((void *)pager->system_chunk.userspace_addr +
					pager->system_chunk.memory_size))) {
			return &pager->system_chunk;
		}

		struct chunk_list *cl = pager->other_chunks;
		while(cl != NULL) {
			struct kvm_userspace_memory_region *region = cl->chunk;
			if((void *)region->userspace_addr <= host_mem_p &&
					host_mem_p < ((void *)region->userspace_addr + region->memory_size)) {
				return region;
			}
			cl = cl->next;
		}

		return NULL;
	}

uint64_t kvm_pager_map_kernel_page(struct kvm_pager *pager, void *host_mem_p,
		int writeable, int executable) {

	uint64_t guest_physical = host_to_guest_physical(pager, host_mem_p);
	uint64_t guest_virtual = (pager->guest_next_free & ~0xFFF) | (guest_physical & 0xFFF);

	uint64_t *pt_entry = kvm_pager_page_table_walk(pager, guest_virtual,
			writeable, executable, 1);
	if(pt_entry == NULL) {
		return -EIO;
	}

	while(entry_exists(pt_entry)) {
		pt_entry++;
		guest_virtual = guest_virtual + 0x1000;
		if(((uint64_t)pt_entry & ~0xFFF) == (uint64_t)pt_entry) {
			/*this page table seems to be completely full, try the next one */
			guest_virtual = guest_virtual + 0x100000;
			pt_entry = kvm_pager_page_table_walk(pager, guest_virtual,
					writeable, executable, 1);
		}
	}

  /*
   * TODO setting this page up for user makes interrupts work,
   * fix this!
   */
	int err = kvm_pager_create_entry(pager, pt_entry, guest_physical,
			writeable, executable);
  if(err) {
    return err;
  }

	return guest_virtual;
}

int kvm_pager_create_mapping(struct kvm_pager *pager, void *host_mem_p,
		uint64_t guest_virtual, int writeable, int executable) {
	int err;

	assert(pager->system_chunk.userspace_addr != 0);
	assert((host_mem_p < pager->host_pml4_p) ||
			host_mem_p >= (pager->host_pml4_p + 0x400000));

	/* sanity checks on the host, we need 4MB to fit all possible page maps */
	if(pager->system_chunk.memory_size < ELKVM_SYSTEM_MEMSIZE) {
		return -EIO;
	}

	/* sanity checks on the offset */
	if(((uint64_t)host_mem_p & 0xFFF) != (guest_virtual & 0xFFF)) {
		return -EIO;
	}

	uint64_t guest_physical = host_to_guest_physical(pager, host_mem_p);
	uint64_t *pt_entry = kvm_pager_page_table_walk(pager, guest_virtual,
			writeable, executable, 1);

	/* do NOT overwrite existing page table entries! */
	if(entry_exists(pt_entry)) {
		if((*pt_entry & ~0xFFF) != (guest_physical & ~0xFFF)) {
			return -1;
		}
		/* TODO check if flags are the same */
		return 0;
	}

	err = kvm_pager_create_entry(pager, pt_entry, guest_physical,
			writeable, executable);

	return err;
}

void *kvm_pager_get_host_p(struct kvm_pager *pager, uint64_t guest_virtual) {
	uint64_t *entry = kvm_pager_page_table_walk(pager, guest_virtual, 0, 0, 0);
	if(entry == NULL) {
		return NULL;
	}

  struct kvm_userspace_memory_region *chunk = NULL;
  uint64_t guest_physical = (*entry & 0x000FFFFFFFFFF000) | (guest_virtual & 0xFFF);
  if(guest_physical < pager->system_chunk.guest_phys_addr ||
      pager->system_chunk.guest_phys_addr +
      pager->system_chunk.memory_size <= guest_physical) {
    struct chunk_list *cl = pager->other_chunks;
    while(guest_physical < cl->chunk->guest_phys_addr ||
        cl->chunk->guest_phys_addr + cl->chunk->memory_size < guest_physical) {
      assert(cl->next != NULL);
      cl = cl->next;
    }
    chunk = cl->chunk;
  } else {
    chunk = &pager->system_chunk;
  }
	return (void *)((guest_physical - chunk->guest_phys_addr) + chunk->userspace_addr);
}

uint64_t *kvm_pager_page_table_walk(struct kvm_pager *pager, uint64_t guest_virtual,
		int write, int execute, int create) {
	uint64_t *table_base = (uint64_t *)pager->host_pml4_p;
	/* we should always have paging in place, when this gets called! */
	assert(table_base != NULL);

	uint64_t *entry = NULL;
	int addr_low = 39;
	int addr_high = 47;

	for(int i = 0; i < 4; i++) {
		entry = kvm_pager_find_table_entry(pager, table_base,
				guest_virtual, addr_low, addr_high);
		addr_low -= 9;
		addr_high -= 9;
		if(!entry_exists(entry)) {
			if(!create) {
				return NULL;
			}
			if(create && i < 3) {
				int err = kvm_pager_create_table(pager, entry, write, execute);
				if(err) {
					return NULL;
				}
			}
		}
		if(i < 3) {
			if(write && !(*entry & 0x2)) {
				if(create) {
					*entry |= 0x2;
				}
			}
			if(execute && (*entry & PT_BIT_NXE)) {
				if(create) {
					*entry &= ~PT_BIT_NXE;
				}
			}
			table_base = kvm_pager_find_next_table(pager, entry);
		}
	}

	return entry;
}

uint64_t *kvm_pager_find_next_table(struct kvm_pager *pager,
		uint64_t *host_tbl_entry_p) {
	if(!entry_exists(host_tbl_entry_p)) {
		return NULL;
	}

	/* location of the next table is in bits 12 - 51 of the entry */
	uint64_t guest_next_tbl = *host_tbl_entry_p & 0x000FFFFFFFFFF000;
	return (uint64_t *)(pager->system_chunk.userspace_addr + guest_next_tbl);
}

uint64_t *kvm_pager_find_table_entry(struct kvm_pager *pager,
		uint64_t *host_tbl_base_p, uint64_t guest_virtual, int off_low, int off_high) {
	uint64_t off = (guest_virtual << (63 - off_high)) >> ((63 - off_high) + off_low);

	uint64_t *entry = host_tbl_base_p + off;
	return entry;
}

int kvm_pager_create_table(struct kvm_pager *pager, uint64_t *host_entry_p,
		int writeable, int executable) {

	uint64_t guest_next_tbl = host_to_guest_physical(pager, pager->host_next_free_tbl_p);
	if(guest_next_tbl == 0) {
		return -EIO;
	}
	memset(pager->host_next_free_tbl_p, 0, 0x1000);
	pager->host_next_free_tbl_p += 0x1000;
	int err = kvm_pager_create_entry(pager, host_entry_p, guest_next_tbl,
			writeable, executable);
	return err;
}

int kvm_pager_create_entry(struct kvm_pager *pager, uint64_t *host_entry_p,
		uint64_t guest_next, int writeable, int executable) {
	/* save base address of next tbl in entry */
	*host_entry_p = guest_next & ~0xFFF;

	/* TODO give this method a flag for marking pages as user mode */
	*host_entry_p |= 0x4;

	if(writeable) {
		*host_entry_p |= 0x2;
	}

	if(!executable) {
		*host_entry_p |= PT_BIT_NXE;
	}

	/* mark the entry as present */
	*host_entry_p |= 0x1;

	return 0;
}

int kvm_pager_set_brk(struct kvm_pager *pager, uint64_t guest_addr) {
  pager->brk_addr = guest_addr;
  return 0;
}

int kvm_pager_handle_pagefault(struct kvm_pager *pager, uint64_t pfla,
    uint32_t err_code) {

		void *host_p = kvm_pager_get_host_p(pager, pfla);
		if(host_p != NULL) {
			kvm_pager_dump_page_tables(pager);
			printf("\n Invalid");
		}
		printf(" Page Fault:\n");
		printf(" -------------------\n");
		printf(" PFLA: 0x%016lx, expected host address: %p\n", pfla, host_p);
		uint64_t page_off = pfla & 0xFFF;
		uint64_t pt_off   = (pfla >> 12) & 0x1FF;
		uint64_t pd_off   = (pfla >> 21) & 0x1FF;
		uint64_t pdpt_off = (pfla >> 30) & 0x1FF;
		uint64_t pml4_off = (pfla >> 39) & 0x1FF;
		printf(" Offsets: PML4: %3lu PDPT: %3lu PD: %3lu PT: %3lu Page: %4lu\n",
				pml4_off, pdpt_off, pd_off, pt_off, page_off);

    if(err_code >= 0) {
      printf("\n");
      printf(" Page Fault Error Code:\n");
      printf(" ----------------------\n");
      printf(" P: %1x R/W: %1x U/S: %1x RSV: %1x I/D: %1x\n",
          err_code & 0x1,
          (err_code >> 1) & 0x1,
          (err_code >> 2) & 0x1,
          (err_code >> 3) & 0x1,
          (err_code >> 4) & 0x1);
    }

    return 1;
}

void kvm_pager_dump_page_tables(struct kvm_pager *pager) {
	printf(" Page Tables:\n");
	printf(" ------------\n");

	kvm_pager_dump_table(pager, pager->host_pml4_p, 4);
	printf(" ------------\n");
	return;
}

void kvm_pager_dump_table(struct kvm_pager *pager, void *host_p, int level) {
	if(level < 1) {
		return;
	}

	char *tname;
	switch(level) {
		case 1: tname = "Page Table\0";
						break;
		case 2: tname = "Page Directory\0";
						break;
		case 3: tname = "Page Directory Pointer Table\0";
						break;
		case 4: tname = "PML4\0";
						break;
		default: tname = "Invalid Level\0";
						 break;
	}

	uint64_t *entry = host_p;
	void *present[512];
	int entries = 0;

	uint64_t guest_physical = host_to_guest_physical(pager, host_p);
	printf(" %s with host base %p (0x%lx)\n", tname, host_p, guest_physical);
	printf(" Offset P W Us WTC C A 6-8 9-11\tNext\t\tNXE\n");

	for(int i = 0; i < 512; i++) {
		if(*entry & 0x1) {
			uint64_t entry_guest_physical = *entry & 0xFFFFFFFFFF000;
			printf(" %3i    %1lx %1lx  %1lx   %1lx %1lx %1lx   %1lx    %1lx\t%011lx\t%1lx\n",
					i,
					*entry & 0x1,
					(*entry & 0x2) >> 1,
					(*entry & 0x4) >> 2,
					(*entry & 0x8) >> 3,
					(*entry & 0x10) >> 4,
					(*entry & 0x20) >> 5,
					(*entry >> 6) & 0x7,
					(*entry >> 9) & 0x7,
					entry_guest_physical,
					(*entry >> 63));
			present[entries++] = (void *)entry_guest_physical +
				pager->system_chunk.userspace_addr;
		}
		entry++;
	}
	printf(" --------\n");
	printf("\n");

	if(level > 1) {
		for(int i = 0; i<entries; i++) {
			kvm_pager_dump_table(pager, present[i], level-1);
		}
	}
	return;
}
