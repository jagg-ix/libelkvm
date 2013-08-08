#include <elkvm.h>
#include <gdt.h>
#include <tss.h>
#include <vcpu.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

int elkvm_gdt_setup(struct kvm_vm *vm) {
	
	memset(vm->region[MEMORY_REGION_GDT].host_base_p, 0,
		 sizeof(struct elkvm_gdt_segment_descriptor));

	struct elkvm_gdt_segment_descriptor *entry = 
		vm->region[MEMORY_REGION_GDT].host_base_p +
		sizeof(struct elkvm_gdt_segment_descriptor);

	/* code segment */
	elkvm_gdt_create_segment_descriptor(entry, 0x0, 0xFFFFFFFF,
			GDT_SEGMENT_READABLE | GDT_SEGMENT_EXECUTABLE | GDT_SEGMENT_BIT |
			GDT_SEGMENT_PRESENT | GDT_SEGMENT_DIRECTION_BIT,
			GDT_SEGMENT_PAGE_GRANULARITY | GDT_SEGMENT_LONG);

	entry++;

	/* data segment */
	elkvm_gdt_create_segment_descriptor(entry, 0x0, 0xFFFFFFFF,
			GDT_SEGMENT_WRITEABLE | GDT_SEGMENT_BIT | GDT_SEGMENT_PRESENT,
			GDT_SEGMENT_PAGE_GRANULARITY | GDT_SEGMENT_PROTECTED_32 );

	entry++;

	/* stack segment */
	elkvm_gdt_create_segment_descriptor(entry, 0x0, 0xFFFFFFFF,
			GDT_SEGMENT_WRITEABLE | GDT_SEGMENT_BIT | GDT_SEGMENT_PRESENT,
			GDT_SEGMENT_PAGE_GRANULARITY | GDT_SEGMENT_PROTECTED_32 );

	entry++;

	/* task state segment */
	uint64_t tss_base = vm->region[MEMORY_REGION_GDT].guest_virtual + GDT_NUM_ENTRIES * 8;
	elkvm_gdt_create_segment_descriptor(entry,
			tss_base & 0xFFFFFFFF,
			sizeof(struct elkvm_tss64),
			0x9 | GDT_SEGMENT_PRESENT,
			GDT_SEGMENT_PROTECTED_32);
	/*
	 * tss entry has 128 bits, make a second entry to account for that
	 * the upper part of base is in the beginning of that second entry
	 * rest is ignored or must be 0, just set everything to 0
	 */
	entry++;
	uint64_t *upper_tss = (uint64_t *)entry;
	*upper_tss = tss_base >> 32;

	uint64_t tr_selector = (uint64_t)entry -
		(uint64_t)vm->region[MEMORY_REGION_GDT].host_base_p;
	
	elkvm_tss_setup64(
			vm,
			vm->region[MEMORY_REGION_GDT].host_base_p + GDT_NUM_ENTRIES * 8,
			0x10);

	struct kvm_vcpu *vcpu = vm->vcpus->vcpu;

	int err = kvm_vcpu_get_regs(vcpu);
	if(err) {
		return err;
	}

	vcpu->sregs.gdt.base = vm->region[MEMORY_REGION_GDT].guest_virtual;
	vcpu->sregs.gdt.limit = GDT_NUM_ENTRIES * 8 - 1;

	vcpu->sregs.tr.base = vm->region[MEMORY_REGION_GDT].guest_virtual +
		GDT_NUM_ENTRIES * 8;
	vcpu->sregs.tr.limit = sizeof(struct elkvm_tss64);
	vcpu->sregs.tr.selector = tr_selector;

	err = kvm_vcpu_set_regs(vcpu);
	if(err) {
		return err;
	}

	err = kvm_pager_create_mapping(&vm->pager, 
			vm->region[MEMORY_REGION_GDT].host_base_p,
			vm->region[MEMORY_REGION_GDT].guest_virtual);
	if(err) {
		return err;
	}

	return 0;
}

int elkvm_gdt_create_segment_descriptor(struct elkvm_gdt_segment_descriptor *entry,
	uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {

	if(base & 0xFFF00000) {
		return -EINVAL;
	}

	entry->base1        = base  & 0xFFFF;
	entry->base2        = (base >> 16) & 0xFF;
	entry->base3        = (base >> 24);
	entry->limit1       = limit & 0xFFFF;
	entry->limit2_flags = ((limit >> 16) & 0xF) | ((uint16_t)flags << 4);
	entry->access       = access;

	return 0;
}

int elkvm_calc_segment_regs(struct kvm_vcpu *vcpu, struct elkvm_gdt_segment_descriptor *entry, 
		int num_entries) {
	return -1;
}

void elkvm_gdt_dump(struct kvm_vm *vm) {

	printf("\n Global Descriptor Table:\n");
	printf(  " ------------------------\n");
	printf(  " selector\tbase\tlimit\taccess\tflags\n");

	for(int i = 0; i < GDT_NUM_ENTRIES; i++) {
		struct elkvm_gdt_segment_descriptor *entry = vm->region[MEMORY_REGION_GDT].host_base_p +
			i * sizeof(struct elkvm_gdt_segment_descriptor);
		uint16_t selector = i * sizeof(struct elkvm_gdt_segment_descriptor);

		printf(" 0x%4x\t\t0x%08x\t0x%05x\t0x%02x\t0x%1x\n", selector, gdt_base(entry),
			gdt_limit(entry), entry->access, gdt_flags(entry));
	}

	printf("\n");
}
