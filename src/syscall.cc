#include <cstring>
#include <iostream>

#include <errno.h>
#include <asm-generic/fcntl.h>
#include <asm/prctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <elkvm.h>
#include <elkvm-internal.h>
#include <heap.h>
#include <mapping.h>
#include <syscall.h>
#include <vcpu.h>

#include "elfloader.h"
#include "region.h"

int elkvm_handle_hypercall(Elkvm::VM &vmi, std::shared_ptr<struct kvm_vcpu> vcpu) {

  int err = 0;

  uint64_t call = Elkvm::get_hypercall_type(vmi, vcpu);
  switch(call) {
    case ELKVM_HYPERCALL_SYSCALL:
      err = elkvm_handle_syscall(vmi, vcpu.get());
      break;
    case ELKVM_HYPERCALL_INTERRUPT:
      err = elkvm_handle_interrupt(vmi, vcpu.get());
      if(err) {
        return err;
      }
      break;
    default:
      fprintf(stderr,
          "Hypercall was something else, don't know how to handle, ABORT!\n");
      return 1;
  }

  if(err) {
    return err;
  }

  elkvm_emulate_vmcall(vcpu.get());

  err = elkvm_signal_deliver(vmi);
  assert(err == 0);

  return 0;
}

int elkvm_handle_interrupt(Elkvm::VM &vmi, struct kvm_vcpu *vcpu) {
  uint64_t interrupt_vector = vcpu->pop();

  if(vmi.debug_mode()) {
    printf(" INTERRUPT with vector 0x%lx detected\n", interrupt_vector);
    kvm_vcpu_get_sregs(vcpu);
    kvm_vcpu_dump_regs(vcpu);
    Elkvm::dump_stack(vmi, vcpu);
  }

  /* Stack Segment */
  if(interrupt_vector == 0x0c) {
    uint64_t err_code = vcpu->pop();
    printf("STACK SEGMENT FAULT\n");
    printf("Error Code: %lu\n", err_code);
    return 1;
  }

  /* General Protection */
  if(interrupt_vector == 0x0d) {
    uint64_t err_code = vcpu->pop();
    printf("GENERAL PROTECTION FAULT\n");
    printf("Error Code: %lu\n", err_code);
    return 1;

  }

  /* page fault */
	if(interrupt_vector == 0x0e) {
    int err = kvm_vcpu_get_sregs(vcpu);
    if(err) {
      return err;
    }

    if(vcpu->sregs.cr2 == 0x0) {
      printf("\n\nABORT: SEGMENTATION FAULT\n\n");
      exit(1);
      return 1;
    }

    uint32_t err_code = vcpu->pop();
    void *hp = vmi.get_region_manager()->get_pager().get_host_p(vcpu->sregs.cr2);
    Elkvm::dump_page_fault_info(vcpu->sregs.cr2, err_code, hp);
    if(hp) {
      vmi.get_region_manager()->get_pager().dump_page_tables();
    }
    if(vcpu->check_pagefault(err_code, vmi.debug_mode())) {
      return 0;
    }

    return 1;
	}

	return 1;
}

/* Taken from uClibc/libc/sysdeps/linux/x86_64/bits/syscalls.h
   The Linux/x86-64 kernel expects the system call parameters in
   registers according to the following table:

   syscall number  rax
   arg 1   rdi
   arg 2   rsi
   arg 3   rdx
   arg 4   r10
   arg 5   r8
   arg 6   r9

   The Linux kernel uses and destroys internally these registers:
   return address from
   syscall   rcx
   additionally clobered: r12-r15,rbx,rbp
   eflags from syscall r11

   Normal function call, including calls to the system call stub
   functions in the libc, get the first six parameters passed in
   registers and the seventh parameter and later on the stack.  The
   register use is as follows:

    system call number in the DO_CALL macro
    arg 1    rdi
    arg 2    rsi
    arg 3    rdx
    arg 4    rcx
    arg 5    r8
    arg 6    r9

   We have to take care that the stack is aligned to 16 bytes.  When
   called the stack is not aligned since the return address has just
   been pushed.


   Syscalls of more than 6 arguments are not supported.  */

int elkvm_handle_syscall(Elkvm::VM &vmi, struct kvm_vcpu *vcpu) {
	uint64_t syscall_num = vcpu->regs.rax;
  if(vmi.debug_mode()) {
    fprintf(stderr, " SYSCALL %3lu detected\n", syscall_num);
  }

	long result;
	if(syscall_num > NUM_SYSCALLS) {
    fprintf(stderr, "\tINVALID syscall_num: %lu\n", syscall_num);
		result = -ENOSYS;
	} else {
    if(vmi.debug_mode()) {
      fprintf(stderr, "(%s)\n", elkvm_syscalls[syscall_num].name);
    }
		result = elkvm_syscalls[syscall_num].func(vmi);
    if(syscall_num == __NR_exit_group) {
      return ELKVM_HYPERCALL_EXIT;
    }
	}
	/* binary expects syscall result in rax */
	vcpu->regs.rax = result;

	return 0;
}

void elkvm_syscall1(std::shared_ptr<struct kvm_vcpu> vcpu, uint64_t *arg) {
	*arg = vcpu->regs.rdi;
}

void elkvm_syscall2(std::shared_ptr<struct kvm_vcpu> vcpu,
		uint64_t *arg1, uint64_t *arg2) {
	*arg1 = vcpu->regs.rdi;
	*arg2 = vcpu->regs.rsi;
}

void elkvm_syscall3(std::shared_ptr<struct kvm_vcpu> vcpu,
		uint64_t *arg1, uint64_t *arg2, uint64_t *arg3) {
	*arg1 = vcpu->regs.rdi;
	*arg2 = vcpu->regs.rsi;
	*arg3 = vcpu->regs.rdx;
}

void elkvm_syscall4(std::shared_ptr<struct kvm_vcpu> vcpu,
		uint64_t *arg1, uint64_t *arg2, uint64_t *arg3, uint64_t *arg4) {
	*arg1 = vcpu->regs.rdi;
	*arg2 = vcpu->regs.rsi;
	*arg3 = vcpu->regs.rdx;
  *arg4 = vcpu->regs.r10;
}

void elkvm_syscall5(std::shared_ptr<struct kvm_vcpu> vcpu,
		uint64_t *arg1, uint64_t *arg2, uint64_t *arg3, uint64_t *arg4,
    uint64_t *arg5) {
	*arg1 = vcpu->regs.rdi;
	*arg2 = vcpu->regs.rsi;
	*arg3 = vcpu->regs.rdx;
  *arg4 = vcpu->regs.r10;
  *arg5 = vcpu->regs.r8;
}

void elkvm_syscall6(std::shared_ptr<struct kvm_vcpu> vcpu,
		uint64_t *arg1, uint64_t *arg2, uint64_t *arg3, uint64_t *arg4,
    uint64_t *arg5, uint64_t *arg6) {
	*arg1 = vcpu->regs.rdi;
	*arg2 = vcpu->regs.rsi;
	*arg3 = vcpu->regs.rdx;
  *arg4 = vcpu->regs.r10;
  *arg5 = vcpu->regs.r8;
  *arg6 = vcpu->regs.r9;
}

long elkvm_do_read(Elkvm::VM &vmi) {
	if(vmi.get_handlers()->read == NULL) {
		printf("READ handler not found\n");
		return -ENOSYS;
	}
  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

	uint64_t fd;
	uint64_t buf_p = 0x0;
	char *buf;
	uint64_t count;

	elkvm_syscall3(vcpu, &fd, &buf_p, &count);

  assert(buf_p != 0x0);
	buf = reinterpret_cast<char *>(vmi.get_region_manager()->get_pager().get_host_p(buf_p));

  uint64_t bend_p = buf_p + count - 1;
  void *bend = vmi.get_region_manager()->get_pager().get_host_p(bend_p);
  long result = 0;

  if(!vmi.get_region_manager()->same_region(buf, bend)) {
    assert(vmi.get_region_manager()->host_address_mapped(bend));
    char *host_begin_mark = NULL;
    char *host_end_mark = buf;
    uint64_t mark_p = buf_p;
    ssize_t current_count = count;
    do {
      host_begin_mark = reinterpret_cast<char *>(vmi.get_region_manager()->get_pager().get_host_p(mark_p));
      std::shared_ptr<Elkvm::Region> region = vmi.get_region_manager()->find_region(host_begin_mark);
      if(mark_p != buf_p) {
        assert(host_begin_mark == region->base_address());
      }

      host_end_mark = reinterpret_cast<char *>(region->last_valid_address());
      assert(host_end_mark > host_begin_mark);

      ssize_t newcount = host_end_mark - host_begin_mark;
      if(newcount > current_count) {
        newcount = current_count;
      }

      long in_result = vmi.get_handlers()->read((int)fd, host_begin_mark, newcount);
      if(in_result < 0) {
        return errno;
      }
      if(in_result < newcount) {
        return result + in_result;
      }
      if(vmi.debug_mode()) {
        printf("\n============ LIBELKVM ===========\n");
        printf("READ from fd: %i with size 0x%lx of 0x%lx buf 0x%lx (%p)\n",
            (int)fd, newcount, count, buf_p, buf);
        printf("RESULT %li (0x%lx)\n", result,result);
        printf("=================================\n");
      }

      mark_p += in_result;
      current_count -= in_result;
      result += in_result;
    } while(!vmi.get_region_manager()->same_region(host_begin_mark, bend));
    assert(current_count == 0);

  } else {
    result = vmi.get_handlers()->read((int)fd, buf, (size_t)count);
  }

  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("READ from fd: %i with size 0x%lx buf 0x%lx (%p)\n",
        (int)fd, count, buf_p, buf);
    printf("RESULT %li\n", result);
    printf("=================================\n");
  }

	return result;
}

long elkvm_do_write(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->write == NULL) {
    printf("WRITE handler not found\n");
    return -ENOSYS;
  }
  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  uint64_t fd = 0x0;
  guestptr_t buf_p = 0x0;
  void *buf;
  uint64_t count = 0x0;

  elkvm_syscall3(vcpu, &fd, &buf_p, &count);

  assert(buf_p != 0x0);
  buf = vmi.get_region_manager()->get_pager().get_host_p(buf_p);

  std::shared_ptr<Elkvm::Region> r = vmi.get_region_manager()->find_region(buf);
  assert(r != nullptr);

  char *current_buf = reinterpret_cast<char *>(buf);
  size_t remaining_count = count;
  ssize_t total = 0;
  while(!r->contains_address(current_buf + remaining_count - 1)) {
    long result = vmi.get_handlers()->write(static_cast<int>(fd),
        current_buf, r->space_after_address(current_buf));
    if(result < 0) {
      return -errno;
    }
    total += result;

    if(vmi.debug_mode()) {
      printf("\n============ LIBELKVM ===========\n");
      printf("SPLIT WRITE to fd: %i with size 0x%lx buf 0x%lx (%p)\n",
          (int)fd, count, buf_p, buf);
      printf("\tcurrent buf: %p remaining bytes: 0x%lx\n",
          current_buf, remaining_count);
      printf("RESULT %li\n", result);
      printf("=================================\n");
    }
    current_buf += result;
    remaining_count -= result;
    r = vmi.get_region_manager()->find_region(current_buf);
  }
  assert(r->contains_address(reinterpret_cast<char *>(buf) + count - 1));

  long result = vmi.get_handlers()->write(static_cast<int>(fd),
      current_buf, remaining_count);
  if(result < 0) {
    return -errno;
  }
  total += result;

  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("WRITE to fd: %i with size 0x%lx buf 0x%lx (%p)\n",
        (int)fd, count, buf_p, buf);
    printf("RESULT %li\n", result);
    printf("=================================\n");
  }

  return total;
}

long elkvm_do_open(Elkvm::VM &vmi) {
	if(vmi.get_handlers()->open == NULL) {
		printf("OPEN handler not found\n");
		return -ENOSYS;
	}
  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

	uint64_t pathname_p = 0x0;
	char *pathname = NULL;
	uint64_t flags = 0x0;
	uint64_t mode = 0x0;

	elkvm_syscall3(vcpu, &pathname_p, &flags, &mode);

  assert(pathname_p != 0x0);
	pathname = reinterpret_cast<char *>(vmi.get_region_manager()->get_pager().get_host_p(pathname_p));

	long result = vmi.get_handlers()->open(pathname, (int)flags, (mode_t)mode);

  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("OPEN file %s with flags %i and mode %x\n", pathname,
			(int)flags, (mode_t)mode);
    printf("RESULT: %li\n", result);
    printf("=================================\n");
  }

	return result;
}

long elkvm_do_close(Elkvm::VM &vmi) {
	if(vmi.get_handlers()->close == NULL) {
		printf("CLOSE handler not found\n");
		return -ENOSYS;
	}
  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

	uint64_t fd = 0;
	elkvm_syscall1(vcpu, &fd);

  if(vmi.debug_mode()) {
    printf("CLOSE file with fd: %li\n", fd);
  }
	long result = vmi.get_handlers()->close((int)fd);

  if(vmi.debug_mode()) {
    printf("RESULT: %li\n", result);
  }

	return result;
}

long elkvm_do_stat(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->stat == NULL) {
    printf("STAT handler not found\n");
    return -ENOSYS;
  }
  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  uint64_t path_p = 0;
  uint64_t buf_p = 0;
  char *path = NULL;
  struct stat *buf;
  elkvm_syscall2(vcpu, &path_p, &buf_p);

  assert(path_p != 0x0);
  path = reinterpret_cast<char *>(vmi.get_region_manager()->get_pager().get_host_p(path_p));
  assert(buf_p != 0x0);
  buf  = reinterpret_cast<struct stat *>(vmi.get_region_manager()->get_pager().get_host_p(buf_p));

  long result = vmi.get_handlers()->stat(path, buf);
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("STAT file %s with buf at: 0x%lx (%p)\n",
        path, buf_p, buf);
    printf("RESULT: %li\n", result);
    printf("=================================\n");
  }

  return result;
}

long elkvm_do_fstat(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->fstat == NULL) {
    printf("FSTAT handler not found\n");
    return -ENOSYS;
  }
  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  uint64_t fd = 0;
  uint64_t buf_p = 0;
  struct stat *buf = NULL;
  elkvm_syscall2(vcpu, &fd, &buf_p);

  assert(buf_p != 0x0);
	buf = reinterpret_cast<struct stat *>(vmi.get_region_manager()->get_pager().get_host_p(buf_p));

  if(vmi.debug_mode()) {
    printf("FSTAT file with fd %li buf at 0x%lx (%p)\n", fd, buf_p, buf);
  }
  long result = vmi.get_handlers()->fstat(fd, buf);

  if(vmi.debug_mode()) {
    printf("RESULT: %li\n", result);
  }

  return result;
}

long elkvm_do_lstat(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->lstat == NULL) {
    printf("LSTAT handler not found\n");
    return -ENOSYS;
  }
  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  uint64_t path_p = 0;
  uint64_t buf_p = 0;
  char *path = NULL;
  struct stat *buf;
  elkvm_syscall2(vcpu, &path_p, &buf_p);

  assert(path_p != 0x0);
  path = reinterpret_cast<char *>(vmi.get_region_manager()->get_pager().get_host_p(path_p));
  assert(buf_p != 0x0);
  buf  = reinterpret_cast<struct stat *>(vmi.get_region_manager()->get_pager().get_host_p(buf_p));

  long result = vmi.get_handlers()->lstat(path, buf);
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("LSTAT file %s with buf at: 0x%lx (%p)\n",
        path, buf_p, buf);
    printf("RESULT: %li\n", result);
    printf("=================================\n");
  }

  return result;
}

long elkvm_do_poll(Elkvm::VM &vmi __attribute__((unused))) {
	return -ENOSYS;
}

long elkvm_do_lseek(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->lseek == NULL) {
    printf("LSEEK handler not found\n");
    return -ENOSYS;
  }

  uint64_t fd;
  uint64_t off;
  uint64_t whence;
  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  elkvm_syscall3(vcpu, &fd, &off, &whence);

  long result = vmi.get_handlers()->lseek(fd, off, whence);
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("LSEEK fd %lu offset %lu whence %lu\n",
        fd, off, whence);
    printf("RESULT: %li\n", result);
    printf("=================================\n");

  }
  return result;


}

long elkvm_do_mmap(Elkvm::VM &vmi) {
  /* obtain a region_mapping and fill this with a proposal
   * on how to do the mapping */
  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);
  guestptr_t addr = 0x0;
  uint64_t length = 0x0;
  uint64_t prot   = 0x0;
  uint64_t flags  = 0x0;
  uint64_t fd     = 0;
  uint64_t off    = 0;

  elkvm_syscall6(vcpu, &addr, &length, &prot, &flags, &fd, &off);

  /* create a mapping object with the data from the user, this will
   * also allocate the memory for this mapping */
  Elkvm::Mapping &mapping =
    vmi.get_heap_manager().get_mapping(addr, length, prot, flags, fd, off);


  /* if a handler is specified, call the monitor for corrections etc. */
  long result = 0;
  if(vmi.get_handlers()->mmap_before != NULL) {
    struct region_mapping *cm = mapping.c_mapping();
    result = vmi.get_handlers()->mmap_before(cm);
    /* write changes back to mapping obj */
    const int remap = mapping.diff(cm);
    if(remap) {
      int err = vmi.get_heap_manager().unmap(mapping);
      assert(err == 0 && "could not unmap mapping");
    }
    mapping.sync_back(cm);
    if(remap) {
      int err = vmi.get_heap_manager().map(mapping);
      assert(err == 0 && "could not map mapping");
    }
    delete(cm);
  }

  /* now do the standard actions not handled by the monitor
   * i.e. copy data for file-based mappings, split existing mappings for
   * MAP_FIXED if necessary etc. */

  if(!mapping.anonymous()) {
    mapping.fill();
  }

  /* call the monitor again, so it can do what has been left */
  if(vmi.get_handlers()->mmap_after != NULL) {
    result = vmi.get_handlers()->mmap_after(mapping.c_mapping());
  }

  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("MMAP addr 0x%lx length %lu (0x%lx) prot %lu flags %lu",
        addr, length, length, prot, flags);
    if(!(flags & MAP_ANONYMOUS)) {
      printf(" fd %lu offset %li", fd, off);
    }
    if(flags & MAP_FIXED) {
      printf(" MAP_FIXED");
    }
    printf("\n");
    print(std::cout, mapping);

    printf("RESULT: %li\n", result);
    printf("=================================\n");
  }
  if(result < 0) {
    return -errno;
  }

  return mapping.guest_address();
}

long elkvm_do_mprotect(Elkvm::VM &vmi) {
  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  guestptr_t addr = 0;
  uint64_t len = 0;
  uint64_t prot = 0;
  elkvm_syscall3(vcpu, &addr, &len, &prot);

  assert(page_aligned(addr) && "mprotect address must be page aligned");
  if(!vmi.get_heap_manager().address_mapped(addr)) {
    vmi.get_heap_manager().dump_mappings();
    vmi.get_region_manager()->dump_regions();
    std::cout << "mprotect with invalid address: 0x" << std::hex
      << addr << std::endl;
    return -EINVAL;
  }

  Elkvm::Mapping &mapping = vmi.get_heap_manager().find_mapping(addr);
  int err = 0;
  if(mapping.get_length() != len) {
    /* we need to split this mapping */
    vmi.get_heap_manager().slice(mapping, addr, len);
    std::shared_ptr<Elkvm::Region> r = vmi.get_region_manager()->allocate_region(len);
    Elkvm::Mapping new_mapping(r, addr, len, prot, mapping.get_flags(),
        mapping.get_fd(), mapping.get_offset());
    vmi.get_heap_manager().map(new_mapping);
    vmi.get_heap_manager().add_mapping(new_mapping);
  } else {
    /* only modify this mapping */
    mapping.mprotect(prot);
    err = vmi.get_heap_manager().map(mapping);
  }

  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("MPROTECT reguested with address: 0x%lx len: 0x%lx prot: %i\n",
        addr, len, (int)prot);
    print(std::cout, mapping);
    printf("RESULT: %i\n", err);
    printf("=================================\n");
  }

	return err;
}

long elkvm_do_munmap(Elkvm::VM &vmi) {
  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  guestptr_t addr_p = 0;
  void *addr = NULL;
  uint64_t length = 0;
  elkvm_syscall2(vcpu, &addr_p, &length);

  if(addr_p != 0x0) {
    addr = vmi.get_region_manager()->get_pager().get_host_p(addr_p);
  }

  Elkvm::Mapping &mapping = vmi.get_heap_manager().find_mapping(addr);
  vmi.get_heap_manager().unmap(mapping, addr_p, pages_from_size(length));

  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("MUNMAP reguested with address: 0x%lx (%p) length: 0x%lx\n",
        addr_p, addr, length);
    if(!mapping.all_unmapped()) {
      print(std::cout, mapping);
    }
    printf("=================================\n");
  }

  return 0;

}

long elkvm_do_brk(Elkvm::VM &vmi) {
  guestptr_t user_brk_req = 0;
  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);
  elkvm_syscall1(vcpu, &user_brk_req);

  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("BRK reguested with address: 0x%lx current brk address: 0x%lx\n",
        user_brk_req, vmi.get_heap_manager().get_brk());
  }

  /* if the requested brk address is 0 just return the current brk address */
  if(user_brk_req == 0) {
    if(vmi.debug_mode()) {
      printf("=================================\n");
    }
    return vmi.get_heap_manager().get_brk();
  }

  int err = vmi.get_heap_manager().brk(user_brk_req);
  if(vmi.debug_mode()) {
    printf("BRK done: err: %i (%s) newbrk: 0x%lx\n",
        err, strerror(err), vmi.get_heap_manager().get_brk());
    printf("=================================\n");
  }
  if(err) {
    return err;
  }
  return vmi.get_heap_manager().get_brk();
}

long elkvm_do_sigaction(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->sigaction == NULL) {
    printf("SIGACTION handler not found\n");
    return -ENOSYS;
  }

  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);
  uint64_t signum;
  uint64_t act_p;
  uint64_t oldact_p;

  elkvm_syscall3(vcpu, &signum, &act_p, &oldact_p);

  struct sigaction *act = NULL;
  struct sigaction *oldact = NULL;
  if(act_p != 0x0) {
    act = reinterpret_cast<struct sigaction *>(vmi.get_region_manager()->get_pager().get_host_p(act_p));
  }
  if(oldact_p != 0x0) {
    oldact = reinterpret_cast<struct sigaction *>(vmi.get_region_manager()->get_pager().get_host_p(oldact_p));
  }

  int err = 0;
  if(vmi.get_handlers()->sigaction((int)signum, act, oldact)) {
    err = elkvm_signal_register(vmi, (int)signum, act, oldact);
  }

  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf(" SIGACTION with signum: %i act: 0x%lx (%p) oldact: 0x%lx (%p)\n",
        (int)signum, act_p, act, oldact_p, oldact);
    if(err != 0) {
      printf("ERROR: %i\n", errno);
    }
    printf("=================================\n");

  }

  return err;
}

long elkvm_do_sigprocmask(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->sigprocmask == NULL) {
    printf("SIGPROCMASK handler not found\n");
    return -ENOSYS;
  }

  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  uint64_t how;
  uint64_t set_p;
  uint64_t oldset_p;

  elkvm_syscall3(vcpu, &how, &set_p, &oldset_p);

  sigset_t *set = NULL;
  sigset_t *oldset = NULL;
  if(set_p != 0x0) {
    set = reinterpret_cast<sigset_t *>(vmi.get_region_manager()->get_pager().get_host_p(set_p));
  }
  if(oldset_p != 0x0) {
    oldset = reinterpret_cast<sigset_t *>(vmi.get_region_manager()->get_pager().get_host_p(oldset_p));
  }

  long result = vmi.get_handlers()->sigprocmask(how, set, oldset);
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("RT SIGPROCMASK with how: %i (%p) set: 0x%lx (%p) oldset: 0x%lx (%p)\n",
        (int)how, &how, set_p, set, oldset_p, oldset);
    printf("RESULT: %li\n", result);
    if(result < 0) {
      printf("ERROR No: %i Msg: %s\n", errno, strerror(errno));
    }
    printf("=================================\n");

  }
  return result;
}

long elkvm_do_sigreturn(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_ioctl(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_pread64(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_pwrite64(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

void elkvm_get_host_iov(Elkvm::VM &vmi __attribute__((unused)),
    uint64_t iov_p, uint64_t iovcnt, struct iovec *host_iov) {
  struct iovec *guest_iov = NULL;
  assert(iov_p != 0x0);
  guest_iov = reinterpret_cast<struct iovec *>
    (vmi.get_region_manager()->get_pager().get_host_p(iov_p));

  for(unsigned i = 0; i < iovcnt; i++) {
    assert(guest_iov[i].iov_base != NULL);
    host_iov[i].iov_base = vmi.get_region_manager()->get_pager().get_host_p(
        reinterpret_cast<guestptr_t>(guest_iov[i].iov_base));
    host_iov[i].iov_len  = guest_iov[i].iov_len;
  }

}

long elkvm_do_readv(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->readv == NULL) {
    printf("READV handler not found\n");
    return -ENOSYS;
  }
  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  uint64_t fd = 0;
  uint64_t iov_p = 0;
  uint64_t iovcnt = 0;

  elkvm_syscall3(vcpu, &fd, &iov_p, &iovcnt);

  struct iovec host_iov[iovcnt];
  elkvm_get_host_iov(vmi, iov_p, iovcnt, host_iov);

  long result = vmi.get_handlers()->readv(fd, host_iov, iovcnt);
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("READV with fd: %i (%p) iov: 0x%lx iovcnt: %i\n",
        (int)fd, &fd, iov_p, (int)iovcnt);
    printf("RESULT: %li\n", result);
    if(result < 0) {
      printf("ERROR No: %i Msg: %s\n", errno, strerror(errno));
    }
    printf("=================================\n");
  }
  return result;
}

long elkvm_do_writev(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->writev == NULL) {
    printf("WRITEV handler not found\n");
    return -ENOSYS;
  }
  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  uint64_t fd = 0;
  uint64_t iov_p = 0;
  uint64_t iovcnt = 0;

  elkvm_syscall3(vcpu, &fd, &iov_p, &iovcnt);

  struct iovec host_iov[iovcnt];
  elkvm_get_host_iov(vmi, iov_p, iovcnt, host_iov);

  long result = vmi.get_handlers()->writev(fd, host_iov, iovcnt);
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("WRITEV with fd: %i iov: 0x%lx iovcnt: %i\n",
        (int)fd, iov_p, (int)iovcnt);
    printf("RESULT: %li\n", result);
    printf("=================================\n");
  }
  return result;
}

long elkvm_do_access(Elkvm::VM &vmi) {
	if(vmi.get_handlers()->access == NULL) {
    printf("ACCESS handler not found\n");
		return -ENOSYS;
	}
  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  uint64_t path_p;
  uint64_t mode;

  elkvm_syscall2(vcpu, &path_p, &mode);

  assert(path_p != 0x0);
  char *pathname = reinterpret_cast<char *>(vmi.get_region_manager()->get_pager().get_host_p(path_p));
  if(pathname == NULL) {
    return -EFAULT;
  }

  long result = vmi.get_handlers()->access(pathname, mode);
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("ACCESS with pathname: %s (0x%lx) mode: %i\n",
      pathname, path_p, (int)mode);
    printf("RESULT: %li\n", result);
    printf("=================================\n");
  }

  if(result) {
    return -errno;
  }

  return 0;
}

long elkvm_do_pipe(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->pipe == NULL) {
    printf("PIPE handler not found\n");
    return -ENOSYS;
  }

  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  uint64_t pipefd_p = 0x0;
  int *pipefd = NULL;

  elkvm_syscall1(vcpu, &pipefd_p);

  pipefd = reinterpret_cast<int *>(vmi.get_region_manager()->get_pager().get_host_p(pipefd_p));
  assert(pipefd != NULL);

  long result = vmi.get_handlers()->pipe(pipefd);
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("PIPE with pipefds at: %p (0x%lx)\n",
        pipefd, pipefd_p);
    printf("RESULT: %li\n", result);
    printf("=================================\n");
  }
  if(result) {
    return -errno;
  }

  return 0;
}

long elkvm_do_select(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_sched_yield(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_mremap(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_msync(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_mincore(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_madvise(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_shmget(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_shmat(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_shmctl(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_dup(Elkvm::VM &vmi) {
	if(vmi.get_handlers()->dup == NULL) {
    printf("DUP handler not found\n");
		return -ENOSYS;
	}
  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  uint64_t oldfd;

  elkvm_syscall1(vcpu, &oldfd);

  if(vmi.debug_mode()) {
    printf("CALLING DUP handler with oldfd %i\n",
      (int)oldfd);
  }

  long result = vmi.get_handlers()->dup(oldfd);
  if(vmi.debug_mode()) {
    printf("DUP result: %li\n", result);
  }

  return -errno;
}

long elkvm_do_dup2(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_pause(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_nanosleep(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->nanosleep == NULL) {
    printf("NANOSLEEP handler not found\n");
    return -ENOSYS;
  }

  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  uint64_t req_p;
  uint64_t rem_p;
  elkvm_syscall2(vcpu, &req_p, &rem_p);

  struct timespec *req = NULL;
  struct timespec *rem = NULL;

  if(req_p != 0x0) {
    req = reinterpret_cast<struct timespec *>(vmi.get_region_manager()->get_pager().get_host_p(req_p));
  }
  if(rem_p != 0x0) {
    rem = reinterpret_cast<struct timespec *>(vmi.get_region_manager()->get_pager().get_host_p(rem_p));
  }

  long result = vmi.get_handlers()->nanosleep(req, rem);
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("NANOSLEEP\n");
    printf("RESULT: %li\n", result);
    printf("=================================\n");
  }

  return result;
}

long elkvm_do_getitimer(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_alarm(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_setitimer(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_getpid(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->getpid == NULL) {
    return -ENOSYS;
  }

  long pid = vmi.get_handlers()->getpid();
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("GETPID\n");
    printf("RESULT: %li\n", pid);
    printf("=================================\n");
  }

  return pid;
}

long elkvm_do_sendfile(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_socket(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_connect(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_accept(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_sendto(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_recvfrom(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_sendmsg(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_recvmsg(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_shutdown(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_bind(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_listen(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_getsockname(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_getpeername(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_socketpair(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_setsockopt(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_getsockopt(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_clone(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_fork(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_vfork(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_execve(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_exit(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_wait4(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_kill(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_uname(Elkvm::VM &vmi) {
	if(vmi.get_handlers()->uname == NULL) {
		return -ENOSYS;
	}
  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

	struct utsname *buf = NULL;
	uint64_t bufp = 0;
	elkvm_syscall1(vcpu, &bufp);

  assert(bufp != 0x0);
	buf = (struct utsname *)vmi.get_region_manager()->get_pager().get_host_p(bufp);
  assert(buf != NULL && "host buffer address cannot be NULL in uname");

	long result = vmi.get_handlers()->uname(buf);
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("UNAME buf at: 0x%lx (%p)\n", bufp, buf);
    printf("syname: %s nodename: %s release: %s version: %s machine: %s\n",
			buf->sysname, buf->nodename, buf->release, buf->version, buf->machine);
    printf("RESULT: %li\n", result);
    printf("=================================\n");
  }
	return result;
}

long elkvm_do_semget(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_semop(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_semctl(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_shmdt(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_msgget(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_msgsnd(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_msgrcv(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_msgctl(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_fcntl(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->fcntl == NULL) {
    printf("FCNTL handler not found\n");
    return -ENOSYS;
  }

  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  uint64_t fd = 0;
  uint64_t cmd = 0;
  /*
   * depending on the value of cmd arg is either an int or a pointer
   * to a struct flock or a pointer to a struct f_owner_ex
   */
  uint64_t arg_p = 0;

  elkvm_syscall3(vcpu, &fd, &cmd, &arg_p);

  long result = 0;
  switch(cmd) {
    case F_GETOWN_EX:
    case F_SETOWN_EX:
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW: {
      /* NULL statement */;
      void *arg = vmi.get_region_manager()->get_pager().get_host_p(arg_p);
      result = vmi.get_handlers()->fcntl(fd, cmd, arg);
      break;
                   }
    default:
      result = vmi.get_handlers()->fcntl(fd, cmd, arg_p);
      break;
  }

  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("FCNTL with fd: %lu cmd: %lu arg_p: 0x%lx\n",
        fd, cmd, arg_p);
    printf("RESULT: %li\n", result);
    if(result < 0) {
      printf("ERROR No: %i Msg: %s\n", errno, strerror(errno));
    }
    printf("=================================\n");
  }

  return result;
}

long elkvm_do_flock(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_fsync(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_fdatasync(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_truncate(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->truncate == NULL) {
    printf("TRUNCATE handler not found\n");
    return -ENOSYS;
  }

  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  uint64_t path_p = 0;
  uint64_t length;
  char *path = NULL;

  elkvm_syscall2(vcpu, &path_p, &length);

  path = reinterpret_cast<char *>(vmi.get_region_manager()->get_pager().get_host_p(path_p));
  long result = vmi.get_handlers()->truncate(path, length);
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("TRUNCATE with path at: %p (%s) length %lu\n",
        path, path, length);
    printf("RESULT: %li\n", result);
    if(result < 0) {
      printf("ERROR No: %i Msg: %s\n", errno, strerror(errno));
    }
    printf("=================================\n");
  }
  return result;
}

long elkvm_do_ftruncate(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->ftruncate == NULL) {
    printf("FTRUNCATE handler not found\n");
    return -ENOSYS;
  }

  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  uint64_t fd = 0;
  uint64_t length;

  elkvm_syscall2(vcpu, &fd, &length);

  long result = vmi.get_handlers()->ftruncate(fd, length);
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("FTRUNCATE with fd: %lu length %lu\n",
        fd, length);
    printf("RESULT: %li\n", result);
    if(result < 0) {
      printf("ERROR No: %i Msg: %s\n", errno, strerror(errno));
    }
    printf("=================================\n");
  }
  return result;
}

long elkvm_do_getdents(Elkvm::VM &vmi __attribute__((unused))) {
  if(vmi.get_handlers()->getdents == NULL) {
    std::cout << "GETDENTS handler not found\n";
    return -ENOSYS;
  }

  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  uint64_t fd = 0;
  guestptr_t dirp_p = 0x0;
  uint64_t count = 0;

  elkvm_syscall3(vcpu, &fd, &dirp_p, &count);

  struct linux_dirent *dirp = NULL;
  if(dirp_p != 0x0) {
    dirp = reinterpret_cast<struct linux_dirent *>(
        vmi.get_region_manager()->get_pager().get_host_p(dirp_p));
  }

  int res = vmi.get_handlers()->getdents(fd, dirp, count);
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("GETDENTS with fd: %u dirp: 0x%lx (%p) count %u\n",
        (unsigned)fd, dirp_p, dirp, (unsigned)count);
    printf("RESULT: %i\n", res);
    if(res < 0) {
      printf("ERROR No: %i Msg: %s\n", errno, strerror(errno));
    }
    printf("=================================\n");
  }
  if(res < 0) {
    return -errno;
  }
  return res;
}

long elkvm_do_getcwd(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->getcwd == NULL) {
    printf("GETCWD handler not found\n");
    return -ENOSYS;
  }

  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  uint64_t buf_p = 0;
  uint64_t size = 0;
  char *buf = NULL;

  elkvm_syscall2(vcpu, &buf_p, &size);

  buf = reinterpret_cast<char *>(vmi.get_region_manager()->get_pager().get_host_p(buf_p));

  char *result = vmi.get_handlers()->getcwd(buf, size);
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("GETCWD with buf at: 0x%lx (%p) size %lu\n",
        buf_p, buf, size);
    printf("RESULT: %p (%s)\n", result, result);
    if(result == NULL) {
      printf("ERROR No: %i Msg: %s\n", errno, strerror(errno));
    }
    printf("=================================\n");
  }
  if(result == NULL) {
    return errno;
  } else {
    return 0;
  }
}

long elkvm_do_chdir(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_fchdir(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_rename(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_mkdir(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->mkdir == NULL) {
    printf("MKDIR handler not found\n");
    return -ENOSYS;
  }

  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  uint64_t pathname_p = 0;
  uint64_t mode = 0;
  char *pathname = NULL;

  elkvm_syscall2(vcpu, &pathname_p, &mode);

  assert(pathname_p != 0x0);
  pathname = reinterpret_cast<char *>(vmi.get_region_manager()->get_pager().get_host_p(pathname_p));
  long result = vmi.get_handlers()->mkdir(pathname, mode);
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("MKDIR with pathname at: %p (%s) mode %lu\n",
        pathname, pathname, mode);
    printf("RESULT: %li\n", result);
    if(result < 0) {
      printf("ERROR No: %i Msg: %s\n", errno, strerror(errno));
    }
    printf("=================================\n");
  }
  return result;

}

long elkvm_do_rmdir(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_creat(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_link(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_unlink(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->unlink == NULL) {
    printf("UNLINK handler not found\n");
    return -ENOSYS;
  }
  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  uint64_t pathname_p = 0;
  char *pathname = NULL;

  elkvm_syscall1(vcpu, &pathname_p);

  assert(pathname_p != 0x0);
  pathname = reinterpret_cast<char *>(vmi.get_region_manager()->get_pager().get_host_p(pathname_p));
  long result = vmi.get_handlers()->unlink(pathname);
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("UNLINK with pathname at: %p (%s)\n",
        pathname, pathname);
    printf("RESULT: %li\n", result);
    if(result < 0) {
      printf("ERROR No: %i Msg: %s\n", errno, strerror(errno));
    }
    printf("=================================\n");
  }
  return result;
}

long elkvm_do_symlink(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_readlink(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->readlink == NULL) {
    printf("READLINK handler not found\n");
    return -ENOSYS;
  }

  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  uint64_t path_p = 0;
  uint64_t buf_p = 0;
  uint64_t bufsiz = 0;
  char *path = NULL;
  char *buf = NULL;

  elkvm_syscall3(vcpu, &path_p, &buf_p, &bufsiz);

  path = reinterpret_cast<char *>(vmi.get_region_manager()->get_pager().get_host_p(path_p));
  buf  = reinterpret_cast<char *>(vmi.get_region_manager()->get_pager().get_host_p(buf_p));
  long result = vmi.get_handlers()->readlink(path, buf, bufsiz);
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("READLINK with path at: %p (%s) buf at: %p bufsize: %lu\n",
        path, path, buf, bufsiz);
    printf("RESULT: %li\n", result);
    if(result < 0) {
      printf("ERROR No: %i Msg: %s\n", errno, strerror(errno));
    }
    printf("=================================\n");
  }
  return result;
}

long elkvm_do_chmod(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_fchmod(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_chown(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_fchown(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_lchown(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_umask(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_gettimeofday(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->gettimeofday == NULL) {
    return -ENOSYS;
  }

  uint64_t tv_p = 0;
  uint64_t tz_p = 0;
  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);
  elkvm_syscall2(vcpu, &tv_p, &tz_p);

  struct timeval *tv = NULL;
  struct timezone *tz = NULL;

  if(tv_p != 0x0) {
    tv = reinterpret_cast<struct timeval *>(vmi.get_region_manager()->get_pager().get_host_p(tv_p));
  }
  if(tz_p != 0x0) {
    tz = reinterpret_cast<struct timezone *>(vmi.get_region_manager()->get_pager().get_host_p(tz_p));
  }

  long result = vmi.get_handlers()->gettimeofday(tv, tz);
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("GETTIMEOFDAY with timeval: %lx (%p) timezone: %lx (%p)\n",
        tv_p, tv, tz_p, tz);
    printf("RESULT: %li\n", result);
    if(result == 0) {
      if(tv != NULL) {
        printf("timeval: tv_sec: %lu tv_usec: %lu\n", tv->tv_sec, tv->tv_usec);
      }
      if(tz != NULL) {
        printf("timezone: tz_minuteswest: %i tz_dsttime %i\n",
          tz->tz_minuteswest, tz->tz_dsttime);
      }
    } else {
      printf("ERROR No: %i Msg: %s\n", errno, strerror(errno));
    }
    printf("=================================\n");
  }

  return result;
}

long elkvm_do_getrlimit(Elkvm::VM &vmi) {
  /* XXX implement again! */
  return -ENOSYS;
//  uint64_t resource = 0x0;
//  uint64_t rlim_p = 0x0;
//  struct rlimit *rlim = NULL;
//
//  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);
//  elkvm_syscall2(vcpu, &resource, &rlim_p);
//
//  assert(rlim_p != 0x0);
//  rlim = reinterpret_cast<struct rlimit *>(vmi.get_region_manager()->get_pager().get_host_p(rlim_p));
//
//  memcpy(rlim, &vm->rlimits[resource], sizeof(struct rlimit));
//  if(vmi.debug_mode()) {
//    printf("\n============ LIBELKVM ===========\n");
//    printf("GETRLIMIT with resource: %li rlim: 0x%lx (%p)\n",
//        resource, rlim_p, rlim);
//    printf("=================================\n");
//  }
//
//  return 0;
}

long elkvm_do_getrusage(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->getrusage == NULL) {
    printf("GETRUSAGE handler not found\n");
    return -ENOSYS;
  }

  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  uint64_t who = 0;
  uint64_t usage_p = 0x0;
  struct rusage *usage = NULL;

  elkvm_syscall2(vcpu, &who, &usage_p);

  assert(usage_p != 0x0);
  assert(who == RUSAGE_SELF);

  usage = reinterpret_cast<struct rusage *>(vmi.get_region_manager()->get_pager().get_host_p(usage_p));

  long result = vmi.get_handlers()->getrusage(who, usage);
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("RUSAGE with who: %li usage: %p (0x%lx)\n",
        who, usage, usage_p);
    printf("=================================\n");
  }
  return result;
}

long elkvm_do_sysinfo(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_times(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->times == NULL) {
    printf("TIMES handler not found\n");
    return -ENOSYS;
  }
  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  uint64_t buf_p = 0x0;
  struct tms *buf = NULL;

  elkvm_syscall1(vcpu, &buf_p);
  assert(buf_p != 0x0);

  buf = reinterpret_cast<struct tms *>(vmi.get_region_manager()->get_pager().get_host_p(buf_p));
  assert(buf != NULL);

  long result = vmi.get_handlers()->times(buf);
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("TIMES with buf: 0x%lx (%p)\n",
        buf_p, buf);
    printf("Result: %li\n", result);
    if(result >= 0) {
      printf("utime: %li stime: %li cutime: %li cstime: %li\n",
          buf->tms_utime, buf->tms_stime, buf->tms_cutime, buf->tms_cstime);
    }
    printf("=================================\n");
  }

  if(result == -1) {
    return -errno;
  } else {
    return result;
  }
}

long elkvm_do_ptrace(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_getuid(Elkvm::VM &vmi) {
	if(vmi.get_handlers()->getuid == NULL) {
		printf("GETUID handler not found\n");
		return -ENOSYS;
	}

	long result = vmi.get_handlers()->getuid();
  if(vmi.debug_mode()) {
    printf("GETUID RESULT: %li\n", result);
  }

	return result;
}

long elkvm_do_syslog(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_getgid(Elkvm::VM &vmi) {
	if(vmi.get_handlers()->getgid == NULL) {
		printf("GETGID handler not found\n");
		return -ENOSYS;
	}

	long result = vmi.get_handlers()->getgid();
  if(vmi.debug_mode()) {
    printf("GETGID RESULT: %li\n", result);
  }

	return result;
}

long elkvm_do_setuid(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_setgid(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_geteuid(Elkvm::VM &vmi) {
	if(vmi.get_handlers()->geteuid == NULL) {
		printf("GETEUID handler not found\n");
		return -ENOSYS;
	}

	long result = vmi.get_handlers()->geteuid();
  if(vmi.debug_mode()) {
    printf("GETEUID RESULT: %li\n", result);
  }

	return result;
}

long elkvm_do_getegid(Elkvm::VM &vmi) {
	if(vmi.get_handlers()->getegid == NULL) {
		printf("GETEGID handler not found\n");
		return -ENOSYS;
	}

	long result = vmi.get_handlers()->getegid();
  if(vmi.debug_mode()) {
    printf("GETEGID RESULT: %li\n", result);
  }

	return result;
}

long elkvm_do_setpgid(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_getppid(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_getpgrp(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_setsid(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_setreuid(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_setregid(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_getgroups(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_setgroups(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_setresuid(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_getresuid(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_setresgid(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_getresgid(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_getpgid(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_setfsuid(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_setfsgid(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_getsid(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_capget(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_capset(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_rt_sigpending(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_rt_sigtimedwait(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_rt_sigqueueinfo(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_rt_sigsuspend(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_sigaltstack(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_utime(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_mknod(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_uselib(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_personality(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_ustat(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_statfs(Elkvm::VM &vmi __attribute__((unused))) {
  if(vmi.get_handlers()->statfs == NULL) {
    std::cout << "STATFS handler not found\n";
    return -ENOSYS;
  }

  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);
  guestptr_t path_p = 0x0;
  guestptr_t buf_p = 0x0;

  elkvm_syscall2(vcpu, &path_p, &buf_p);

  char *path = NULL;
  struct statfs *buf = NULL;
  if(path_p != 0x0) {
    path = reinterpret_cast<char *>(vmi.get_region_manager()->get_pager().get_host_p(path_p));
  }
  if(buf_p != 0x0) {
    buf = reinterpret_cast<struct statfs *>(
        vmi.get_region_manager()->get_pager().get_host_p(buf_p));
  }

  int res = vmi.get_handlers()->statfs(path, buf);
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("STATFS path 0x%lx (%p) buf 0x%lx (%p)",
        path_p, path, buf_p, buf);
    printf("RESULT: %i\n", res);
    printf("=================================\n");
  }

  if(res == 0) {
    return 0;
  }
  return -errno;
}

long elkvm_do_fstatfs(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_sysfs(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_getpriority(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_setpriority(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_sched_setparam(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_sched_getparam(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_sched_setscheduler(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_sched_getscheduler(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_sched_get_priority_max(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_sched_get_priority_min(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_sched_rr_get_interval(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_mlock(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_munlock(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_mlockall(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_munlockall(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_vhangup(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_modify_ldt(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_pivot_root(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_sysctl(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_prctl(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_arch_prctl(Elkvm::VM &vmi) {
  uint64_t code = 0;
  uint64_t user_addr = 0;
  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  int err = kvm_vcpu_get_sregs(vcpu.get());
  if(err) {
    return err;
  }

  elkvm_syscall2(vcpu, &code, &user_addr);
  assert(user_addr != 0x0);

  uint64_t *host_addr = reinterpret_cast<uint64_t *>(
      vmi.get_region_manager()->get_pager().get_host_p(user_addr));
  assert(host_addr != nullptr && "could not get host address in arch_prctl");

  switch(code) {
    case ARCH_SET_FS:
      vcpu->sregs.fs.base = user_addr;
      break;
    case ARCH_GET_FS:
      *host_addr = vcpu->sregs.fs.base;
      break;
    case ARCH_SET_GS:
      vcpu->sregs.gs.base = user_addr;
      break;
    case ARCH_GET_GS:
      *host_addr = vcpu->sregs.gs.base;
      break;
    default:
      return -EINVAL;
  }

  err = kvm_vcpu_set_sregs(vcpu.get());
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("ARCH PRCTL with code %i user_addr 0x%lx (%p)\n",
        (int)code, user_addr, host_addr);
    printf("RESULT %li\n", err);
    printf("=================================\n");
  }
  return err;
}

long elkvm_do_adjtimex(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_setrlimit(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_chroot(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_sync(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_acct(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_settimeofday(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_mount(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_umount2(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_swapon(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_swapoff(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_reboot(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_sethostname(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_setdomainname(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_iopl(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_ioperm(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_create_module(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_init_module(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_delete_module(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_get_kernel_syms(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_query_module(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_quotactl(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_nfsservctl(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_getpmsg(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_putpmsg(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_afs_syscall(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_tuxcall(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_security(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_gettid(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->gettid == NULL) {
    printf("GETTID handler not found\n");
    return -ENOSYS;
  }

  long result = vmi.get_handlers()->gettid();
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("GETTID\n");
    printf("RESULT: %li\n", result);
    printf("=================================\n");
  }
  return result;
}

long elkvm_do_readahead(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_setxattr(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_lsetxattr(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_fsetxattr(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_getxattr(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_lgetxattr(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_fgetxattr(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_listxattr(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_llistxattr(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_flistxattr(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_removexattr(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_lremovexattr(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_fremovexattr(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_tkill(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_time(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->time == NULL) {
    printf("TIME handler not found\n");
    return -ENOSYS;
  }

  uint64_t time_p = 0;
  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);
  elkvm_syscall1(vcpu, &time_p);

  time_t *time = NULL;
  if(time_p != 0x0) {
    time = reinterpret_cast<time_t *>(vmi.get_region_manager()->get_pager().get_host_p(time_p));
  }

  long result = vmi.get_handlers()->time(time);
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("TIME with arg %lx (%p)\n", time_p, time);
    printf("RESULT: %li\n", result);
    printf("=================================\n");
  }

  return result;
}

long elkvm_do_futex(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->futex == NULL) {
    printf("FUTEX handler not found\n");
    return -ENOSYS;
  }

  uint64_t uaddr_p = 0x0;
  uint64_t op = 0;
  uint64_t val = 0;
  uint64_t timeout_p = 0x0;
  uint64_t uaddr2_p = 0x0;
  uint64_t val3 = 0;
  int *uaddr = NULL;
  const struct timespec *timeout = NULL;
  int *uaddr2 = NULL;

  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);
  elkvm_syscall6(vcpu, &uaddr_p, &op, &val, &timeout_p, &uaddr2_p, &val3);

  if(uaddr_p != 0x0) {
    uaddr = reinterpret_cast<int *>(vmi.get_region_manager()->get_pager().get_host_p(uaddr_p));
  }
  if(timeout_p != 0x0) {
    timeout = reinterpret_cast<const struct timespec *>(vmi.get_region_manager()->get_pager().get_host_p(timeout_p));
  }
  if(uaddr2_p != 0x0) {
    uaddr2 = reinterpret_cast<int *>(vmi.get_region_manager()->get_pager().get_host_p(uaddr2_p));
  }

  printf("FUTEX with uaddr %p (0x%lx) op %lu val %lu timeout %p (0x%lx)"
      " uaddr2 %p (0x%lx) val3 %lu\n",
      uaddr, uaddr_p, op, val, timeout, timeout_p, uaddr2, uaddr2_p, val3);
  long result = vmi.get_handlers()->futex(uaddr, op, val, timeout, uaddr2, val3);
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("FUTEX with uaddr %p (0x%lx) op %lu val %lu timeout %p (0x%lx)"
        " uaddr2 %p (0x%lx) val3 %lu\n",
        uaddr, uaddr_p, op, val, timeout, timeout_p, uaddr2, uaddr2_p, val3);
    printf("RESULT: %li\n", result);
    printf("=================================\n");
  }

  if(result) {
    return -errno;
  }
  return result;

}

long elkvm_do_sched_setaffinity(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_sched_getaffinity(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_set_thread_area(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_io_setup(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_io_destroy(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_getevents(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_submit(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_cancel(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_get_thread_area(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_lookup_dcookie(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_epoll_create(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_epoll_ctl_old(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_epoll_wait_old(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_remap_file_pages(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_getdents64(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_set_tid_address(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_restart_syscall(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_semtimedop(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_fadive64(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_timer_create(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_timer_settime(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_timer_gettime(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_timer_getoverrun(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_timer_delete(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_clock_settime(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_clock_gettime(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->clock_gettime == NULL) {
    printf("CLOCK GETTIME handler not found\n");
    return -ENOSYS;
  }

  uint64_t clk_id = 0x0;
  uint64_t tp_p = 0x0;
  struct timespec *tp = NULL;

  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);
  assert(vcpu != NULL);

  elkvm_syscall2(vcpu, &clk_id, &tp_p);
  assert(tp_p != 0x0);

  tp = reinterpret_cast<struct timespec *>(vmi.get_region_manager()->get_pager().get_host_p(tp_p));
  assert(tp != NULL);

  long result = vmi.get_handlers()->clock_gettime(clk_id, tp);
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("CLOCK GETTIME with clk_id %li tp 0x%lx (%p)\n",
        clk_id, tp_p, tp);
    printf("RESULT: %li\n", result);
    printf("=================================\n");
  }
  return result;
}

long elkvm_do_clock_getres(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_clock_nanosleep(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_exit_group(Elkvm::VM &vmi) {
  uint64_t status = 0;
  elkvm_syscall1(vmi.get_vcpu(0), &status);

  vmi.get_handlers()->exit_group(status);
  /* should not be reached... */
  return -ENOSYS;
}

long elkvm_do_epoll_wait(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_epoll_ctl(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_tgkill(Elkvm::VM &vmi) {
  if(vmi.get_handlers()->tgkill == NULL) {
    printf("TGKILL handler not found\n");
    return -ENOSYS;
  }

  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  uint64_t tgid = 0x0;
  uint64_t tid = 0x0;
  uint64_t sig = 0x0;

  elkvm_syscall3(vcpu, &tgid, &tid, &sig);

  long result = vmi.get_handlers()->tgkill(tgid, tid, sig);
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("TGKILL with tgid %li tid %li sig %li\n", tgid, tid, sig);
    printf("RESULT: %li\n", result);
    printf("=================================\n");
  }
  return result;

}

long elkvm_do_utimes(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_vserver(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_mbind(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_mpolicy(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_get_mempolicy(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_mq_open(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_mq_unlink(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_mq_timedsend(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_mq_timedreceive(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_mq_notify(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_getsetattr(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_kexec_load(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_waitid(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_add_key(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_request_key(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_keyctl(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_ioprio_set(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_ioprio_get(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_inotify_init(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_inotify_add_watch(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_inotify_rm_watch(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_migrate_pages(Elkvm::VM &vmi __attribute__((unused))) {
  return -ENOSYS;
}

long elkvm_do_openat(Elkvm::VM &vmi __attribute__((unused))) {
  if(vmi.get_handlers()->openat == NULL) {
    printf("OPENAT handler not found\n");
    return -ENOSYS;
  }

  std::shared_ptr<struct kvm_vcpu> vcpu = vmi.get_vcpu(0);

  uint64_t dirfd = 0;
  guestptr_t pathname_p = 0x0;
  uint64_t flags = 0;

  elkvm_syscall3(vcpu, &dirfd, &pathname_p, &flags);

  char *pathname = NULL;
  if(pathname_p != 0x0) {
    pathname = reinterpret_cast<char *>(
        vmi.get_region_manager()->get_pager().get_host_p(pathname_p));
  }

  int res = vmi.get_handlers()->openat((int)dirfd, pathname, (int)flags);
  if(vmi.debug_mode()) {
    printf("\n============ LIBELKVM ===========\n");
    printf("OPENAT with dirfd %i pathname 0x%lx (%p) flags %i\n",
        (int)dirfd, pathname_p, pathname, (int)flags);
    printf("RESULT: %i\n", res);
    printf("=================================\n");
  }

  if(res < 0) {
    return -errno;
  }

  return res;
}
