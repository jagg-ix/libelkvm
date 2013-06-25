#pragma once

#include <linux/kvm.h>

#include <elkvm.h>

struct kvm_vcpu {
	int fd;
	struct kvm_regs regs;
	struct kvm_sregs sregs;
};

struct vcpu_list {
	struct kvm_vcpu *vcpu;
	struct vcpu_list *next;
};

/*
	Creates a runnable VCPU in the mode given by the parameter
*/
int kvm_vcpu_create(struct kvm_vm *, int);

/*
	Set the VCPU's rip to a specific value
*/
int kvm_vcpu_set_rip(struct kvm_vcpu *, uint64_t);
