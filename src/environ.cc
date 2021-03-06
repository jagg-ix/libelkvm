//
// libelkvm - A library that allows execution of an ELF binary inside a virtual
// machine without a full-scale operating system
// Copyright (C) 2013-2015 Florian Pester <fpester@os.inf.tu-dresden.de>, Björn
// Döbel <doebel@os.inf.tu-dresden.de>,   economic rights: Technische Universitaet
// Dresden (Germany)
//
// This file is part of libelkvm.
//
// libelkvm is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// libelkvm is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with libelkvm.  If not, see <http://www.gnu.org/licenses/>.
//

#include <cstring>

#include <gelf.h>

#include <elkvm/elfloader.h>
#include <elkvm/elkvm-internal.h>
#include <elkvm/environ.h>
#include <elkvm/kvm.h>
#include <elkvm/region.h>
#include <elkvm/stack.h>
#include <elkvm/elkvm-log.h>
#include <elkvm/vcpu.h>

// the global logger object (see elkvm-log.h)
ElkvmLog globalElkvmLogger;

namespace Elkvm {

  EnvRegion::EnvRegion(std::shared_ptr<Region> r) :
    _region(r),
    _offset(r->size()) {
      _region->set_guest_addr(
        reinterpret_cast<guestptr_t>(_region->base_address()));
    }

  guestptr_t EnvRegion::write_str(const std::string &str) {
    assert(_offset > (str.length() + 1));
    off64_t noff = _offset - str.length() - 1;

    char *target = static_cast<char *>(_region->base_address()) + noff;
    assert((target + str.length())
        < (static_cast<char *>(_region->base_address()) + _region->size()));

    guestptr_t guest_virtual = _region->guest_address() + noff;
    assert((guest_virtual + str.length())
        < (_region->guest_address() + _region->size()));

    str.copy(target, str.length());
    _offset = noff;
    return guest_virtual;
  }

  Environment::Environment(const ElfBinary &bin, std::shared_ptr<Region> reg,
      int argc, char **argv, char **env) :
    _region(reg),
    _auxv(),
    _env(),
    _argv(),
    _argc(argc),
    binary(bin)
  {
    fill_argv(argv),
    fill_env(env);
    auto auxv = calc_auxv(env);
    fill_auxv(auxv);
  }

  Elf64_auxv_t *Environment::calc_auxv(char **env) const {
    /* XXX this breaks, if we do not get the original envp */

    /* traverse past the environment */
    char **auxv_p = env;
    while(*auxv_p != NULL) {
      auxv_p++;
    }
    auxv_p++;
    return reinterpret_cast<Elf64_auxv_t *>(auxv_p);
  }

  void Environment::fill_argv(char **argv) {
    for(int i = 0; i < _argc; i++) {
      _argv.emplace_back(argv[i]);
    }
  }

  void Environment::fill_env(char **env) {
    while(*env != nullptr) {
      _env.emplace_back(*env);
      env++;
    }
  }

  void Environment::fill_auxv(Elf64_auxv_t *auxv) {
    while(auxv->a_type != AT_NULL) {
      _auxv.push_back(*auxv);
      auxv++;
    }
  }

  void Environment::fix_auxv_dynamic_values() {
    short all_set = 0;
    for(auto &a : _auxv) {
      /*
       * if the binary is dynamically linked, we need to reset these types
       * so the dynamic linker loads the correct values
       */
        switch(a.a_type) {

          case AT_PHDR:
            a.a_un.a_val = binary.get_auxv().at_phdr;
            all_set |= 0x1;
            break;
          case AT_PHENT:
            a.a_un.a_val = binary.get_auxv().at_phent;
            all_set |= 0x2;
            break;
          case AT_PHNUM:
            a.a_un.a_val = binary.get_auxv().at_phnum;
            all_set |= 0x4;
            break;
          case AT_ENTRY:
            a.a_un.a_val = binary.get_auxv().at_entry;
            all_set |= 0x8;
            break;
          case AT_BASE:
            a.a_un.a_val = binary.get_auxv().at_base;
            all_set |= 0x10;
            break;
        }
    }
    assert(all_set == 0x1F && "elf auxv is complete");
  }

  bool Environment::treat_as_int_type(int type) const {
    std::vector<int> itypes({
          AT_NULL,
          AT_IGNORE,
          AT_EXECFD,
          AT_PHDR,
          AT_PHENT,
          AT_PHNUM,
          AT_PAGESZ,
          AT_FLAGS,
          AT_ENTRY,
          AT_NOTELF,
          AT_UID,
          AT_EUID,
          AT_GID,
          AT_EGID,
          AT_HWCAP,
          AT_CLKTCK,
          AT_SECURE,
          AT_BASE,
        });
    auto it = std::find(itypes.begin(), itypes.end(), type);
    return it != itypes.end();
  }

  bool Environment::ignored_type(int type) const {
    std::vector<int> ignored({
        AT_SYSINFO_EHDR
        });
    auto it = std::find(std::begin(ignored), std::end(ignored), type);
    return it != ignored.end();
  }

  void Environment::push_auxv_raw(VCPU &vcpu) {
    for(auto it = _auxv.rbegin(); it != _auxv.rend(); it++) {
      const auto &auxv  = *it;
      if(!ignored_type(auxv.a_type)) {
        if(treat_as_int_type(auxv.a_type)) {
          vcpu.push(auxv.a_un.a_val);
        } else {
          push_str_copy(vcpu, std::string(reinterpret_cast<char *>(auxv.a_un.a_val)));
        }
        vcpu.push(auxv.a_type);
      }
    }
  }

  void Environment::push_auxv(VCPU& vcpu) {
    if(binary.get_auxv().valid) {
      fix_auxv_dynamic_values();
    }
    push_auxv_raw(vcpu);
    vcpu.push(0);
  }

  void Environment::push_env(VCPU& vcpu) {
    for(auto it = _env.rbegin(); it != _env.rend(); it++) {
      push_str_copy(vcpu, *it);
    }
    vcpu.push(0);
  }

  void Environment::push_argv(VCPU& vcpu) {
    for(auto it = _argv.rbegin(); it != _argv.rend(); it++) {
      push_str_copy(vcpu, *it);
    }
  }

  void Environment::push_argc(VCPU &vcpu) const {
    vcpu.push(_argc);
  }

  void Environment::push_str_copy(VCPU& vcpu, const std::string &str) {
    auto guest_virtual = _region.write_str(str);
    vcpu.push(guest_virtual);
  }

int Environment::create(VCPU& vcpu) {
  int err = vcpu.get_regs();
  assert(err == 0 && "error getting vcpu");

  push_auxv(vcpu);
  push_env(vcpu);
  push_argv(vcpu);
  push_argc(vcpu);

  /* if the binary is dynamically linked we need to ajdust some stuff */
  //if(binary.is_dynamically_linked()) {
  //  push_str_copy(*vcpu, bytes, binary.get_loader());
  //  opts->argc++;
  //}

  err = vcpu.set_regs();
  return err;
}

//namespace Elkvm
}
