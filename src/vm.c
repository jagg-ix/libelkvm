#include <linux/kvm.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stropts.h>
#include <sys/stat.h>
#include <unistd.h>

#include <elkvm.h>
#include <idt.h>
#include <kvm.h>
#include <pager.h>
#include <stack.h>
#include <vcpu.h>

int kvm_vm_create(struct elkvm_opts *opts, struct kvm_vm *vm, int mode, int cpus, int memory_size) {
	int err = 0;

	if(opts->fd <= 0) {
		return -EIO;
	}

	vm->fd = ioctl(opts->fd, KVM_CREATE_VM, 0);
	if(vm->fd < 0) {
		return -errno;
	}

	vm->run_struct_size = ioctl(opts->fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	if(vm->run_struct_size < 0) {
		return -EIO;
	}

	for(int i = 0; i < cpus; i++) {
		err = kvm_vcpu_create(vm, mode);
		if(err) {
			return err;
		}
	}

	err = kvm_pager_initialize(vm, mode);
	if(err) {
		return err;
	}

	err = kvm_vm_map_chunk(vm, &vm->pager.system_chunk);
	if(err) {
		return err;
	}

	/* set up the region info for the idt */
	vm->region[MEMORY_REGION_IDT].region_size = 0x1000;
	uint64_t region_offset = vm->pager.system_chunk.memory_size - 
		vm->region[MEMORY_REGION_PTS].region_size - 
		vm->region[MEMORY_REGION_GDT].region_size - 
		vm->region[MEMORY_REGION_IDT].region_size;
	vm->region[MEMORY_REGION_IDT].host_base_p = 
		(void *)vm->pager.system_chunk.userspace_addr + region_offset;
	vm->region[MEMORY_REGION_IDT].guest_virtual = ADDRESS_SPACE_TOP -
		vm->region[MEMORY_REGION_IDT].region_size + 0x1;
	vm->region[MEMORY_REGION_IDT].grows_downward = 0;

	err = elkvm_idt_setup(vm);
	if(err) {
		return err;
	}

	err = kvm_pager_create_mem_chunk(&vm->pager, memory_size, ELKVM_USER_CHUNK_OFFSET);
	if(err) {
		return err;
	}

	err = kvm_vm_map_chunk(vm, vm->pager.other_chunks->chunk);
	if(err) {
		return err;
	}

	err = elkvm_initialize_stack(opts, vm);
	if(err) {
		return err;
	}

	return 0;
}

int kvm_check_cap(struct elkvm_opts *kvm, int cap) {
	if(kvm->fd < 1) {
		return -EIO;
	}

	int r = ioctl(kvm->fd, KVM_CHECK_EXTENSION, cap);
	if(r < 0) {
		return -errno;
	}
	return r;
}

int kvm_vm_vcpu_count(struct kvm_vm *vm) {
	int count = 0;
	struct vcpu_list *vl = vm->vcpus;
	if(vl == NULL) {
		return 0;
	}

	while(vl != NULL) {
		if(vl->vcpu != NULL) {
			count++;
		}
		vl = vl->next;
	}
	return count;
}

int kvm_vm_destroy(struct kvm_vm *vm) {
	return -1;
}

int elkvm_init(struct elkvm_opts *opts, int argc, char **argv, char **environ) {
	opts->argc = argc;
	opts->argv = argv;
	opts->environ = environ;

	opts->fd = open(KVM_DEV_PATH, O_RDWR);
	if(opts->fd < 0) {
		return opts->fd;
	}

	int version = ioctl(opts->fd, KVM_GET_API_VERSION, 0);
	if(version != KVM_EXPECT_VERSION) {
		return -1;
	}

	opts->run_struct_size = ioctl(opts->fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	if(opts->run_struct_size <= 0) {
		return -1;
	}

	return 0;
}

int elkvm_cleanup(struct elkvm_opts *opts) {
	close(opts->fd);
	opts->fd = 0;
	opts->run_struct_size = 0;
	return 0;
}

int elkvm_initialize_stack(struct elkvm_opts *opts, struct kvm_vm *vm) {
	/* for now the region to hold env etc. will be 12 pages large */
	vm->region[MEMORY_REGION_ENV].region_size = 0x12000; 

	uint64_t rsp_offset = vm->pager.system_chunk.memory_size - 
		vm->region[MEMORY_REGION_PTS].region_size - 
		vm->region[MEMORY_REGION_GDT].region_size - 
		vm->region[MEMORY_REGION_IDT].region_size - 
		vm->region[MEMORY_REGION_ENV].region_size;
	vm->region[MEMORY_REGION_ENV].host_base_p = 
		(void *)vm->pager.system_chunk.userspace_addr + rsp_offset;
	vm->region[MEMORY_REGION_ENV].guest_virtual = LINUX_64_STACK_BASE - 
		vm->region[MEMORY_REGION_ENV].region_size;
	vm->region[MEMORY_REGION_ENV].grows_downward = 0;

	vm->region[MEMORY_REGION_STACK].host_base_p = 
		(void *)vm->pager.system_chunk.userspace_addr + rsp_offset;
	vm->region[MEMORY_REGION_STACK].guest_virtual = LINUX_64_STACK_BASE - 
		vm->region[MEMORY_REGION_ENV].region_size;
	vm->region[MEMORY_REGION_STACK].region_size = 0x0;
	vm->region[MEMORY_REGION_STACK].grows_downward = 1;

	int err = kvm_vcpu_get_regs(vm->vcpus->vcpu);
	if(err) {
		return err;
	}

	vm->vcpus->vcpu->regs.rsp = vm->region[MEMORY_REGION_ENV].guest_virtual;

	err = kvm_vcpu_set_regs(vm->vcpus->vcpu);
	if(err) {
		return err;
	}

	err = kvm_pager_create_mapping(&vm->pager, 
			vm->region[MEMORY_REGION_ENV].host_base_p, 
			vm->vcpus->vcpu->regs.rsp);
	if(err) {
		return err;
	}

	void *host_target_p = vm->region[MEMORY_REGION_ENV].host_base_p + 
		vm->region[MEMORY_REGION_ENV].region_size;

	int bytes = elkvm_copy_and_push_str_arr_p(vm, host_target_p, opts->environ);
	host_target_p -= bytes;
	assert(host_target_p > vm->region[MEMORY_REGION_ENV].host_base_p);


	//followed by argv pointers
	bytes = elkvm_copy_and_push_str_arr_p(vm, host_target_p, opts->argv);
	host_target_p -= bytes;
	assert(host_target_p >= vm->region[MEMORY_REGION_ENV].host_base_p);

	//first push argc on the stack
	push_stack(vm, vm->vcpus->vcpu, opts->argc);

	return 0;
}

int elkvm_copy_and_push_str_arr_p(struct kvm_vm *vm, void *host_base_p,
	 	char **str) {
	void *target = host_base_p;
	uint64_t guest_target = host_to_guest_physical(&vm->pager, target);
	int bytes = 0;

	//first push the environment onto the stack
	int i = 0;
	while(str[i]) {
		int len = strlen(str[i]) + 1;
		target -= len;
		bytes += len;

		//copy the data into the vm memory
		strcpy(target, str[i]);
		//TODO copy the trailing NULL byte

		//and push the pointer for the vm
		int err = push_stack(vm, vm->vcpus->vcpu, guest_target);
		if(err) {
			return err;
		}
		i++;
	}

	push_stack(vm, vm->vcpus->vcpu, 0);

	return bytes;
}

int kvm_vm_map_chunk(struct kvm_vm *vm, struct kvm_userspace_memory_region *chunk) {
	int err = ioctl(vm->fd, KVM_SET_USER_MEMORY_REGION, chunk);
//	if(err) {
//		long sz = sysconf(_SC_PAGESIZE);
//		printf("Could not set memory region\n");
//		printf("Error No: %i Msg: %s\n", errno, strerror(errno));
//		printf("Pagesize is: %li\n", sz);
//		printf("Here are some sanity checks that are applied in kernel:\n");
//		int ms = chunk->memory_size & (sz-1);
//		int pa = chunk->guest_phys_addr & (sz-1);
//		int ua = chunk->userspace_addr & (sz-1);
//		printf("memory_size & (PAGE_SIZE -1): %i\n", ms);
//		printf("guest_phys_addr & (PAGE_SIZE-1): %i\n", pa);
//		printf("userspace_addr & (PAGE_SIZE-1): %i\n", ua);
//		printf("TODO verify write access\n");
//	}
	return err;
}

void elkvm_print_regions(struct elkvm_memory_region *regions) {
	printf("\n System Memory Regions:\n");
	printf(" ----------------------\n");
	printf(" Host virtual\t\tGuest virtual\t\tSize\t\t\tD\n");
	for(int i = 0; i < MEMORY_REGION_COUNT; i++) {
		printf("%16p\t0x%016lx\t0x%016lx\t%i\n", regions[i].host_base_p, 
				regions[i].guest_virtual, regions[i].region_size, regions[i].grows_downward);
	}
	printf("\n");
}
