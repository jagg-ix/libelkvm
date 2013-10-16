#pragma once

#include <elkvm.h>
#include <vcpu.h>

struct elkvm_sw_bp {
  uint64_t guest_virtual_addr;
  uint8_t orig_inst;
  uint8_t *host_addr;
  unsigned count;
};

int elkvm_handle_debug(struct kvm_vm *, struct kvm_vcpu *);
int elkvm_set_guest_debug(struct kvm_vcpu *vcpu);

struct elkvm_sw_bp *elkvm_find_bp_for_rip(struct kvm_vcpu *, uint64_t rip);
struct elkvm_sw_bp *elkvm_bp_alloc(uint8_t *host_p, uint64_t rip);

int elkvm_debug_shell(struct kvm_vm *vm);

