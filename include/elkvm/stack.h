#pragma once

#include <environ.h>
#include <region.h>

/* 64bit Linux puts the Stack at 47bits */
#define LINUX_64_STACK_BASE 0x800000000000
#define ELKVM_STACK_GROW    0x200000

namespace Elkvm {

  class VMInternals;

  class Stack {
    private:
      RegionManager &_rm;
      std::vector<std::shared_ptr<Region>> stack_regions;
      std::shared_ptr<Region> kernel_stack;
      struct kvm_vcpu *vcpu;
      int expand();
      guestptr_t base;

    public:
      Stack(RegionManager &rm);
      void init(struct kvm_vcpu *v, const Environment &e);
      int pushq(guestptr_t rsp, uint64_t val);
      uint64_t popq(guestptr_t rsp);
      bool is_stack_expansion(guestptr_t pfla);
      bool grow(guestptr_t pfla);
      guestptr_t kernel_base() const { return kernel_stack->guest_address(); }
      guestptr_t user_base() const { return base; }
  };

  void dump_stack(VMInternals &vmi, struct kvm_vcpu *vcpu);

//namespace Elkvm
}
