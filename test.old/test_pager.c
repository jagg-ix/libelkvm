#include <check.h>

#include <errno.h>
#include <stdlib.h>
#include <stropts.h>

#include <elkvm.h>
#include <pager.h>
#include <vcpu.h>

struct elkvm_opts pager_test_opts;
int vm_fd;
struct kvm_pager pager;
struct kvm_vm the_vm;

void pager_setup() {
	int err = elkvm_init(&pager_test_opts, 0, NULL, NULL);
	assert(err == 0);

	the_vm.fd = ioctl(pager_test_opts.fd, KVM_CREATE_VM, 0);
	assert(the_vm.fd > 0);

  err = elkvm_region_setup(&the_vm);
  assert(err == 0);

	err = kvm_vcpu_create(&the_vm, VM_MODE_X86_64);
	assert(err == 0);
}

void pager_teardown() {
	free(the_vm.root_region->data->host_base_p);
  free(the_vm.root_region);
	elkvm_cleanup(&pager_test_opts);
}

void region_setup() {
	pager.system_chunk.userspace_addr = 0;
	pager.system_chunk.memory_size = 0x400000;
}

void region_teardown() {
}

struct kvm_pager pager;

void memory_setup() {
	int err = posix_memalign(&pager.host_pml4_p, 0x1000, ELKVM_SYSTEM_MEMSIZE);
	assert(err == 0);

	memset(pager.host_pml4_p, 0, ELKVM_SYSTEM_MEMSIZE);
	pager.system_chunk.userspace_addr = (uint64_t)pager.host_pml4_p;
	pager.host_next_free_tbl_p = pager.host_pml4_p + 0x1000;
	pager.system_chunk.memory_size = ELKVM_SYSTEM_MEMSIZE;
	pager.system_chunk.guest_phys_addr = 0x0;
	pager.other_chunks = NULL;

}

void memory_teardown() {
	free(pager.host_pml4_p);
}

START_TEST(test_kvm_pager_initialize_invalid_vm) {
	struct kvm_vm the_vm;
	the_vm.fd = 0;

	int err = kvm_pager_initialize(&the_vm, PAGER_MODE_X86_64);
	ck_assert_int_eq(err, -EIO);
}
END_TEST

START_TEST(test_kvm_pager_initialize_invalid_mode) {
	struct kvm_vm the_vm;
	the_vm.fd = vm_fd;

	int err = kvm_pager_initialize(&the_vm, 9999);
	ck_assert_int_lt(err, 0);
}
END_TEST

START_TEST(test_kvm_pager_initialize_valid) {
	int err = kvm_pager_initialize(&the_vm, PAGER_MODE_X86_64);
	ck_assert_int_eq(err, 0);
}
END_TEST

START_TEST(test_kvm_pager_create_mem_chunk_nopager) {
  int size = 0x400000;
  void *chunk_p;
	int err = kvm_pager_create_mem_chunk(NULL, &chunk_p, size);
	ck_assert_int_eq(err, -EIO);
  ck_assert_ptr_eq(chunk_p, NULL);
	ck_assert_ptr_eq(pager.other_chunks, NULL);
}
END_TEST

START_TEST(test_kvm_pager_create_mem_chunk_valid) {
  int size = 0x400000;
  void *chunk_p;
	int err = kvm_pager_create_mem_chunk(&pager, &chunk_p, size);
	ck_assert_int_eq(err, 0);
  ck_assert_ptr_ne(chunk_p, NULL);
	ck_assert_ptr_ne(pager.other_chunks, NULL);
	//struct chunk_list *cl = pager.other_chunks;
	//ck_assert_int_eq(cl->chunk->memory_size, size);
	//ck_assert_int_eq(cl->chunk->guest_phys_addr, valid_guest_base);
}
END_TEST

START_TEST(test_kvm_pager_create_mem_chunk_mass) {
  ck_abort_msg("Create mass chunks test not implemented");
//  int err = 0;
//  int size = 0x400000;
//  for(int i = 0; i < 100; i++) {
//    void *chunk_p;
//    err = kvm_pager_create_mem_chunk(&pager, &chunk_p, size);
//    ck_assert_int_eq(err, 0);
//    ck_assert_ptr_ne(chunk_p, NULL);
//    cl = pager.other_chunks;
//    ck_assert_ptr_ne(cl, NULL);
//    for(int chunks = 0; chunks < i; chunks++) {
//      ck_assert_ptr_ne(cl->next, NULL);
//      cl = cl->next;
//      if(chunks == i-1) {
//        ck_assert_int_eq(cl->chunk->memory_size, size);
//        //ck_assert_int_eq(cl->chunk->guest_phys_addr, valid_guest_base);
//      }
//    }
//    ck_assert_ptr_eq(cl->next, NULL);
//  }
}
END_TEST

START_TEST(test_kvm_pager_create_page_tables) {
	struct kvm_pager pager;
	int size = 0x400000;

	pager.system_chunk.userspace_addr = (__u64)malloc(size);
	ck_assert_int_ne(pager.system_chunk.userspace_addr, 0);
	pager.host_pml4_p = (void *)pager.system_chunk.userspace_addr;
	pager.system_chunk.memory_size = 0;

	int err = kvm_pager_create_page_tables(&pager, PAGER_MODE_X86_64);
	ck_assert_int_lt(err, 0);

	err = kvm_pager_create_page_tables(NULL, PAGER_MODE_X86_64);
	ck_assert_int_lt(err, 0);

	err = kvm_pager_create_page_tables(&pager, 9999);
	ck_assert_int_lt(err, 0);

	pager.system_chunk.memory_size = size;
	err = kvm_pager_create_page_tables(&pager, PAGER_MODE_X86_64);
	ck_assert_int_eq(err, 0);
	ck_assert_ptr_eq(pager.host_pml4_p, (void *)pager.system_chunk.userspace_addr);
	ck_assert_ptr_eq(pager.host_next_free_tbl_p, pager.host_pml4_p + 0x1000);

	free((void *)pager.system_chunk.userspace_addr);
}
END_TEST

START_TEST(test_kvm_pager_is_invalid_guest_base) {
	struct kvm_pager pager;
	pager.system_chunk.guest_phys_addr = 0x0;
	pager.system_chunk.memory_size = ELKVM_SYSTEM_MEMSIZE;
	pager.other_chunks = NULL;
	uint64_t guest_base = pager.system_chunk.guest_phys_addr;

	int invl = kvm_pager_is_invalid_guest_base(&pager, guest_base);
	ck_assert_int_eq(invl, 1);

	guest_base += ELKVM_SYSTEM_MEMSIZE-1;
	invl = kvm_pager_is_invalid_guest_base(&pager, guest_base);
	ck_assert_int_eq(invl, 1);

	guest_base++;
	invl = kvm_pager_is_invalid_guest_base(&pager, guest_base);
	ck_assert_int_eq(invl, 0);

//	pager.other_chunks = malloc(sizeof(struct chunk_list));
	struct kvm_userspace_memory_region *chunk =
		malloc(sizeof(struct kvm_userspace_memory_region));
	chunk->guest_phys_addr = 0x1000000;
	chunk->memory_size = 0x10000;
//	pager.other_chunks->chunk = chunk;
//	pager.other_chunks->next = NULL;

	invl = kvm_pager_is_invalid_guest_base(&pager, chunk->guest_phys_addr);
	ck_assert_int_eq(invl, 1);

	invl = kvm_pager_is_invalid_guest_base(&pager, chunk->guest_phys_addr + chunk->memory_size- 1);
	ck_assert_int_eq(invl, 1);

	invl = kvm_pager_is_invalid_guest_base(&pager, chunk->guest_phys_addr + chunk->memory_size);
	ck_assert_int_eq(invl, 0);

	invl = kvm_pager_is_invalid_guest_base(&pager, chunk->guest_phys_addr + chunk->memory_size + 0x234);
	ck_assert_int_eq(invl, 1);

	free(chunk);
//	free(pager.other_chunks);
}
END_TEST

START_TEST(test_kvm_pager_append_mem_chunk) {
	struct kvm_pager pager;
	pager.other_chunks = NULL;

	struct kvm_userspace_memory_region r0;
	int count = kvm_pager_append_mem_chunk(&pager, &r0);
	ck_assert_int_eq(count, 0);

	struct kvm_userspace_memory_region r1;
	count = kvm_pager_append_mem_chunk(&pager, &r1);
	ck_assert_int_eq(count, 1);

	struct kvm_userspace_memory_region r2;
	count = kvm_pager_append_mem_chunk(&pager, &r2);
	ck_assert_int_eq(count, 2);

}
END_TEST

START_TEST(test_kvm_pager_find_region_for_host_p_nomem) {
	struct kvm_pager pager;
	pager.other_chunks = NULL;
	pager.system_chunk.userspace_addr = 0;
	pager.system_chunk.memory_size = 0;

	void *p = (void *)0x1000;
	struct kvm_userspace_memory_region *region =
		kvm_pager_find_region_for_host_p(&pager, p);
	ck_assert_ptr_eq(region, NULL);
}
END_TEST

START_TEST(test_kvm_pager_find_region_for_host_p_system) {
	void *p = (void *)0x1000;
	struct kvm_userspace_memory_region *region =
		kvm_pager_find_region_for_host_p(&pager, p);
	ck_assert_ptr_eq(region, &pager.system_chunk);
}
END_TEST

START_TEST(test_kvm_pager_find_region_for_host_p_user) {
  ck_abort_msg("find region for host_p not implemented");
//	struct kvm_userspace_memory_region *chunk = pager.other_chunks->chunk;
//	chunk->userspace_addr = 0x400000;
//	chunk->memory_size = 0x100000;
//
//	void *p = (void *)0x427500;
//	struct kvm_userspace_memory_region *region =
//		kvm_pager_find_region_for_host_p(&pager, p);
//	ck_assert_ptr_eq(region, pager.other_chunks->chunk);
}
END_TEST

START_TEST(test_kvm_pager_find_region_for_host_p_system_edge) {
	void *p = (void *)0x0;
	struct kvm_userspace_memory_region *region =
		kvm_pager_find_region_for_host_p(&pager, p);
	ck_assert_ptr_eq(region, &pager.system_chunk);

	p = (void *)0x400000;
	region = kvm_pager_find_region_for_host_p(&pager, p);
	ck_assert_ptr_eq(region, NULL);
}
END_TEST

START_TEST(test_kvm_pager_find_region_for_host_p_user_edge) {
  ck_abort_msg("find region for host_p not implemented");
//	struct kvm_userspace_memory_region *chunk = pager.other_chunks->chunk;
//	chunk->userspace_addr = 0x400000;
//	chunk->memory_size = 0x100000;
//
//	void *p = (void *)0x400000;
//	struct kvm_userspace_memory_region *region =
//		kvm_pager_find_region_for_host_p(&pager, p);
//	ck_assert_ptr_eq(region, pager.other_chunks->chunk);
//
//	p = (void *)0x500000;
//	region = kvm_pager_find_region_for_host_p(&pager, p);
//	ck_assert_ptr_eq(region, NULL);
}
END_TEST

START_TEST(test_kvm_pager_find_region_for_guest_physical) {

}
END_TEST

START_TEST(test_kvm_pager_create_mapping_invalid_host) {
	struct kvm_pager pager;
	pager.system_chunk.userspace_addr = 0;
	pager.system_chunk.memory_size = 0;

  ptopt_t opts = 0;

	void *p = (void *)0x1000;
	int err = kvm_pager_create_mapping(&pager, p, 0x400000, opts);
	ck_assert_int_eq(err, -1);

}
END_TEST

START_TEST(test_kvm_pager_create_valid_mappings) {
	void *p = (void *)0x400000 + pager.system_chunk.userspace_addr;
	uint64_t guest_virtual_addr = 0x600000;
  ptopt_t opts = 0;
	int err = kvm_pager_create_mapping(&pager, p, guest_virtual_addr, opts);
	ck_assert_int_eq(err, 0);

	void *host_resolved_p = kvm_pager_get_host_p(&pager, guest_virtual_addr);
	ck_assert_ptr_eq(p, host_resolved_p);

	p = (void *)0x400e10 + pager.system_chunk.userspace_addr;
	guest_virtual_addr = 0x400e10;
	err = kvm_pager_create_mapping(&pager, p, guest_virtual_addr, opts);
	ck_assert_int_eq(err, 0);

	host_resolved_p = kvm_pager_get_host_p(&pager, guest_virtual_addr);
	ck_assert_ptr_eq(p, host_resolved_p);

	p = (void *)0x400040 + pager.system_chunk.userspace_addr;
	guest_virtual_addr = 0x800040;
	err = kvm_pager_create_mapping(&pager, p, guest_virtual_addr, opts);
	ck_assert_int_eq(err, 0);

	host_resolved_p = kvm_pager_get_host_p(&pager, guest_virtual_addr);
	ck_assert_ptr_eq(p, host_resolved_p);
}
END_TEST

START_TEST(test_kvm_pager_create_same_mapping) {
	void *p = (void *)0x400000 + pager.system_chunk.userspace_addr;
	uint64_t guest_virtual_addr = 0x600000;
  ptopt_t opts = 0;
	int err = kvm_pager_create_mapping(&pager, p, guest_virtual_addr, opts);
	ck_assert_int_eq(err, 0);

	p = (void *)0x402000 + pager.system_chunk.userspace_addr;
	err = kvm_pager_create_mapping(&pager, p, guest_virtual_addr, opts);
	ck_assert_int_eq(err, -1);
}
END_TEST

START_TEST(test_kvm_pager_create_mapping_invalid_offset) {
	void *p = (void *)0x1e10;
	uint64_t guest_virtual_addr = 0x600000;
  ptopt_t opts = 0;
	int err = kvm_pager_create_mapping(&pager, p, guest_virtual_addr, opts);
	ck_assert_int_eq(err, -EIO);
}
END_TEST

START_TEST(test_kvm_pager_create_mapping_mass) {
	void *p = (void *)pager.system_chunk.userspace_addr + 0x401500;
	uint64_t guest = 0x600500;
  ptopt_t opts = 0;

	int err;
	void *resolved;
	for(int i = 0; i < 1024; i++) {
		err = kvm_pager_create_mapping(&pager, p, guest, opts);
		ck_assert_int_eq(err, 0);

		resolved = kvm_pager_get_host_p(&pager, guest);
		ck_assert_ptr_eq(resolved, p);

		p = p + 0x1000;
		guest = guest + 0x1000;
	}
}
END_TEST

START_TEST(test_kvm_pager_create_entry) {
	pager.system_chunk.guest_phys_addr = 0x0;
	pager.system_chunk.userspace_addr = (uint64_t)pager.host_pml4_p;
	pager.system_chunk.memory_size = 0x2000;

	uint64_t *entry = pager.host_pml4_p + (5 * sizeof(uint64_t));
  ptopt_t opts = PT_OPT_EXEC;
	int err = kvm_pager_create_entry(&pager, entry, 0x1000, opts);
	ck_assert_int_eq(err, 0);
	ck_assert_int_eq(*entry & 0x1, 1);
	ck_assert_uint_eq(*entry & ~0xFFF, 0x1000);
}
END_TEST

START_TEST(test_kvm_pager_find_table_entry) {
	/* this should result in an offset of 2 into the pml4 */
	uint64_t guest_virtual = 0x14000400400;
	uint64_t *expected_entry = pager.host_pml4_p + (2 * sizeof(uint64_t));
	*expected_entry = 0x1001;

	uint64_t *entry = kvm_pager_find_table_entry(&pager, pager.host_pml4_p, guest_virtual, 39, 47);
	ck_assert_ptr_eq(entry, expected_entry);
}
END_TEST

START_TEST(test_kvm_pager_map_kernel_page_valid) {
	int err = kvm_pager_initialize(&the_vm, PAGER_MODE_X86_64);
	ck_assert_int_eq(err, 0);

	uint64_t virt = kvm_pager_map_kernel_page(&the_vm.pager, (void *)0x42, 0, 0);
	ck_assert_int_ne(virt, 0);
}
END_TEST

START_TEST(test_kvm_pager_map_kernel_page_masses) {
	int err = kvm_pager_initialize(&the_vm, PAGER_MODE_X86_64);
	ck_assert_int_eq(err, 0);

	for(uint64_t i = 0; i < 0x5142; i++) {
		uint64_t virt = kvm_pager_map_kernel_page(&the_vm.pager, (void *)i, 0, 0);
		ck_assert_int_ne(virt, 0);
	}
}
END_TEST

Suite *pager_suite() {
	Suite *s = suite_create("Pager");

	TCase *tc_pt_entries = tcase_create("PT entries");
	tcase_add_checked_fixture(tc_pt_entries, memory_setup, memory_teardown);
	tcase_add_test(tc_pt_entries, test_kvm_pager_create_entry);
	tcase_add_test(tc_pt_entries, test_kvm_pager_find_table_entry);
	suite_add_tcase(s, tc_pt_entries);

	TCase *tc_find_region = tcase_create("Find Memory Region");
	tcase_add_checked_fixture(tc_find_region, region_setup, region_teardown);
	tcase_add_test(tc_find_region, test_kvm_pager_find_region_for_host_p_nomem);
	tcase_add_test(tc_find_region, test_kvm_pager_find_region_for_host_p_system);
	tcase_add_test(tc_find_region, test_kvm_pager_find_region_for_host_p_user);
	tcase_add_test(tc_find_region, test_kvm_pager_find_region_for_host_p_system_edge);
	tcase_add_test(tc_find_region, test_kvm_pager_find_region_for_host_p_user_edge);
	suite_add_tcase(s, tc_find_region);

	TCase *tc_create_mappings = tcase_create("Create Virtual Memory Mappings");
	tcase_add_checked_fixture(tc_create_mappings, memory_setup, memory_teardown);
	tcase_add_test_raise_signal(tc_create_mappings,
			test_kvm_pager_create_mapping_invalid_host, 6);
	tcase_add_test(tc_create_mappings, test_kvm_pager_create_valid_mappings);
	tcase_add_test(tc_create_mappings, test_kvm_pager_create_same_mapping);
	tcase_add_test(tc_create_mappings, test_kvm_pager_create_mapping_invalid_offset);
	tcase_add_test(tc_create_mappings, test_kvm_pager_create_mapping_mass);
	suite_add_tcase(s, tc_create_mappings);

	TCase *tc_append_mem_chunk = tcase_create("Append Mem Chunk");
	tcase_add_test(tc_append_mem_chunk, test_kvm_pager_append_mem_chunk);
	suite_add_tcase(s, tc_append_mem_chunk);

	TCase *tc_guest_base = tcase_create("Guest Base");
	tcase_add_test(tc_guest_base, test_kvm_pager_is_invalid_guest_base);
	suite_add_tcase(s, tc_guest_base);

	TCase *tc_page_tables = tcase_create("Create Page Tables");
	tcase_add_test(tc_page_tables, test_kvm_pager_create_page_tables);
	suite_add_tcase(s, tc_page_tables);

	TCase *tc_init = tcase_create("Initialize");
	tcase_add_checked_fixture(tc_init, pager_setup, pager_teardown);
	tcase_add_test(tc_init, test_kvm_pager_initialize_invalid_vm);
	tcase_add_test(tc_init, test_kvm_pager_initialize_invalid_mode);
	tcase_add_test(tc_init, test_kvm_pager_initialize_valid);
	suite_add_tcase(s, tc_init);

	TCase *tc_mem_chunk = tcase_create("Create Mem Chunk");
	tcase_add_test(tc_mem_chunk, test_kvm_pager_create_mem_chunk_nopager);
  tcase_add_test(tc_mem_chunk, test_kvm_pager_create_mem_chunk_valid);
  tcase_add_test(tc_mem_chunk, test_kvm_pager_create_mem_chunk_mass);
  tcase_add_checked_fixture(tc_mem_chunk, pager_setup, pager_teardown);
	suite_add_tcase(s, tc_mem_chunk);

	TCase *tc_map_kernel = tcase_create("Map Kernel Page");
	tcase_add_checked_fixture(tc_map_kernel, pager_setup, pager_teardown);
	tcase_add_test(tc_map_kernel, test_kvm_pager_map_kernel_page_valid);
	tcase_add_test(tc_map_kernel, test_kvm_pager_map_kernel_page_masses);
	suite_add_tcase(s, tc_map_kernel);

	return s;
}