/* * libelkvm - A library that allows execution of an ELF binary inside a virtual
 * machine without a full-scale operating system
 * Copyright (C) 2013-2015 Florian Pester <fpester@os.inf.tu-dresden.de>, Björn
 * Döbel <doebel@os.inf.tu-dresden.de>,   economic rights: Technische Universitaet
 * Dresden (Germany)
 *
 * This file is part of libelkvm.
 *
 * libelkvm is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * libelkvm is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with libelkvm.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <elkvm/config.h>
#include <elkvm/kvm.h>
#include <elkvm/regs.h>
#include <elkvm/stack.h>
#include <elkvm/syscall.h>

#include <linux/kvm.h>

#include <cstdbool>
#include <cstdio>

namespace Elkvm {

class Segment {
  CURRENT_ABI::paramtype _selector;
  CURRENT_ABI::paramtype _base;
  CURRENT_ABI::paramtype _limit;
  CURRENT_ABI::paramtype _type;
  CURRENT_ABI::paramtype _present;
  CURRENT_ABI::paramtype _dpl;
  CURRENT_ABI::paramtype _db;
  CURRENT_ABI::paramtype _s;
  CURRENT_ABI::paramtype _l;
  CURRENT_ABI::paramtype _g;
  CURRENT_ABI::paramtype _avl;

  public:
    Segment(CURRENT_ABI::paramtype base,
            CURRENT_ABI::paramtype limit) :
	  _selector(0),
      _base(base),
      _limit(limit),
	  _type(0),
	  _present(0),
	  _dpl(0),
	  _db(0),
	  _s(0),
	  _l(0),
	  _g(0),
	  _avl(0)
	{}

    Segment(CURRENT_ABI::paramtype selector,
            CURRENT_ABI::paramtype base,
            CURRENT_ABI::paramtype limit,
            CURRENT_ABI::paramtype type,
            CURRENT_ABI::paramtype present,
            CURRENT_ABI::paramtype dpl,
            CURRENT_ABI::paramtype db,
            CURRENT_ABI::paramtype s,
            CURRENT_ABI::paramtype l,
            CURRENT_ABI::paramtype g,
            CURRENT_ABI::paramtype avl) :
      _selector(selector),
      _base(base),
      _limit(limit),
      _type(type),
      _present(present),
      _dpl(dpl),
      _db(db),
      _s(s),
      _l(l),
      _g(g),
      _avl(avl) {}

    CURRENT_ABI::paramtype get_selector() const { return _selector; }
    CURRENT_ABI::paramtype get_base() const { return _base; }
    CURRENT_ABI::paramtype get_limit() const { return _limit; }
    CURRENT_ABI::paramtype get_type() const { return _type; }
    CURRENT_ABI::paramtype is_present() const { return _present; }
    CURRENT_ABI::paramtype get_dpl() const { return _dpl; }
    CURRENT_ABI::paramtype get_db() const { return _db; }
    CURRENT_ABI::paramtype get_s() const { return _s; }
    CURRENT_ABI::paramtype get_l() const { return _l; }
    CURRENT_ABI::paramtype get_g() const { return _g; }
    CURRENT_ABI::paramtype get_avl() const { return _avl; }

    void set_base(CURRENT_ABI::paramtype base) {
      _base = base;
    }

	void set_selector(CURRENT_ABI::paramtype sel) {
      _selector = sel;
	}
};

class VCPU {
  private:
    bool is_singlestepping;
    KVM::VCPU _kvm_vcpu;
    Elkvm::Stack stack;

    void initialize_regs();

  public:
    static const int hypercall_exit = 1;

    VCPU(std::shared_ptr<Elkvm::RegionManager> rm, int vmfd, unsigned cpu_num);
    /*
     * Get VCPU registers from hypervisor
     */
    int get_regs();
    int get_sregs();

    /*
     * Set VCPU registers with hypervisor
     */
    int set_regs();
    int set_sregs();

    /*
     * get and set single registers
     */
    CURRENT_ABI::paramtype get_reg(Elkvm::Reg_t reg) const;
    Elkvm::Segment get_reg(Elkvm::Seg_t seg) const;
    CURRENT_ABI::paramtype get_interrupt_bitmap(unsigned idx) const;
    void set_reg(Elkvm::Reg_t reg, CURRENT_ABI::paramtype val);
    void set_reg(Elkvm::Seg_t seg, const Elkvm::Segment &s);
    void set_entry_point(guestptr_t rip);

    /* MSRs */
    void set_msr(uint32_t idx, CURRENT_ABI::paramtype data);
    CURRENT_ABI::paramtype get_msr(uint32_t idx);

    /* RUNNING the VCPU */
    int run();
    bool handle_vm_exit();

    /* get VCPU hypervisor exit reasons */
    uint32_t exit_reason();
    uint64_t hardware_exit_reason();
    uint64_t hardware_entry_failure_reason();

    /* Debugging */
    int enable_debug();
    int enable_software_breakpoints();
    bool is_singlestep() { return is_singlestepping; }
    int singlestep();
    int singlestep_off();
    std::ostream &print_mmio(std::ostream &os);

    void print_info(const VM &vm);
    /* stack handling */
    CURRENT_ABI::paramtype pop();
    void push(CURRENT_ABI::paramtype val);
    guestptr_t kernel_stack_base() { return stack.kernel_base(); }
    int handle_stack_expansion(uint32_t err, bool debug);
    void init_rsp();
};

std::ostream &print(std::ostream &os, const VCPU &vcpu);
std::ostream &print(std::ostream &os, const std::string &name,
    const Elkvm::Segment &seg);
std::ostream &print(std::ostream &os, const std::string &name,
    struct kvm_dtable dtable);
std::ostream &print_stack(std::ostream &os, const VM &vm, const VCPU &vcpu);

//namespace Elkvm
}

#define VCPU_CR0_FLAG_PAGING            0x80000000
#define VCPU_CR0_FLAG_CACHE_DISABLE     0x40000000
#define VCPU_CR0_FLAG_NOT_WRITE_THROUGH 0x20000000
#define VCPU_CR0_FLAG_PROTECTED         0x1

#define VCPU_CR4_FLAG_OSXSAVE 0x40000
#define VCPU_CR4_FLAG_OSFXSR  0x200
#define VCPU_CR4_FLAG_PAE     0x20
#define VCPU_CR4_FLAG_DE      0x8

#define VCPU_EFER_FLAG_SCE 0x1
#define VCPU_EFER_FLAG_LME 0x100
#define VCPU_EFER_FLAG_LMA 0x400
#define VCPU_EFER_FLAG_NXE 0x800
#define VMX_INVALID_GUEST_STATE 0x80000021
#define CPUID_EXT_VMX      (1 << 5)

#define VCPU_MSR_STAR   0xC0000081
#define VCPU_MSR_LSTAR  0xC0000082
#define VCPU_MSR_CSTAR  0xC0000083
#define VCPU_MSR_SFMASK 0XC0000084

void kvm_vcpu_dump_msr(const std::shared_ptr<Elkvm::VCPU>& vcpu, uint32_t);

/*
 * \brief Returns true if the host supports vmx
*/
bool host_supports_vmx(void);

/*
 * \brief Get the host CPUID
*/
void host_cpuid(uint32_t, uint32_t, uint32_t *, uint32_t *, uint32_t *, uint32_t *);

void print_flags(uint64_t flags);
