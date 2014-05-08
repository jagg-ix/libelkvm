#pragma once

#include <signal.h>
#include <stdbool.h>

#include "vcpu.h"

struct elkvm_signals {

  struct sigaction signals[_NSIG];
  bool handler_active;
  struct kvm_vcpu saved_vcpu;

};

int elkvm_signal_init(struct kvm_vm *vm);
int elkvm_signal_register(struct kvm_vm *vm, int signum, struct sigaction *act,
    struct sigaction *oldact);
int elkvm_signal_deliver(struct kvm_vm *vm);
int elkvm_signal_cleanup(struct kvm_vm *vm);