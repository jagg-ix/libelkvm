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
#include <iostream>

#include <errno.h>
#include <fcntl.h>
#include <asm/prctl.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <unistd.h>

#include <elkvm/elkvm.h>
#include <elkvm/elkvm-internal.h>
#include <elkvm/elfloader.h>
#include <elkvm/heap.h>
#include <elkvm/interrupt.h>
#include <elkvm/mapping.h>
#include <elkvm/syscall.h>
#include <elkvm/vcpu.h>
#include <elkvm/region.h>
#include <elkvm/elkvm-log.h>

X86_64_ABI::paramtype
X86_64_ABI::get_parameter(std::shared_ptr<Elkvm::VCPU> vcpu, unsigned pos)
{
  // no more than 6 params
  assert(pos <= 6);

  switch(pos) {
    case 0:
      return vcpu->get_reg(Elkvm::Reg_t::rax);
    case 1:
      return vcpu->get_reg(Elkvm::Reg_t::rdi);
    case 2:
      return vcpu->get_reg(Elkvm::Reg_t::rsi);
    case 3:
      return vcpu->get_reg(Elkvm::Reg_t::rdx);
    case 4:
      return vcpu->get_reg(Elkvm::Reg_t::r10);
    case 5:
      return vcpu->get_reg(Elkvm::Reg_t::r8);
    case 6:
      return vcpu->get_reg(Elkvm::Reg_t::r9);
  }
  return ~0ULL;
}

void X86_64_ABI::set_syscall_return(std::shared_ptr<Elkvm::VCPU> vcpu, paramtype value)
{
    vcpu->set_reg(Elkvm::Reg_t::rax, value);
}

// XXX: this should be the same for all platforms, we
//      just need to include proper unistd.h
static struct {
  long (*func)(Elkvm::VM *);
  const char *name;
} elkvm_syscalls[NUM_SYSCALLS]
__attribute__((used))
  = {
  [__NR_read]            = { elkvm_do_read, "READ" },
  [__NR_write]           = { elkvm_do_write, "WRITE"},
  [__NR_open]            = { elkvm_do_open, "OPEN"},
  [__NR_close]           = { elkvm_do_close, "CLOSE" },
  [__NR_stat]            = { elkvm_do_stat, "STAT" },
  [__NR_fstat]           = { elkvm_do_fstat, "FSTAT" },
  [__NR_lstat]           = { elkvm_do_lstat, "LSTAT" },
  [__NR_poll]            = { elkvm_do_poll, "POLL" },
  [__NR_lseek]           = { elkvm_do_lseek, "LSEEK" },
  [__NR_mmap]            = { elkvm_do_mmap, "MMAP" },
  [__NR_mprotect]       = { elkvm_do_mprotect, "MPROTECT" },
  [__NR_munmap]         = { elkvm_do_munmap, "MUNMAP" },
  [__NR_brk]            = { elkvm_do_brk, "BRK" },
  [__NR_rt_sigaction]   = { elkvm_do_sigaction, "SIGACTION" },
  [__NR_rt_sigprocmask] = { elkvm_do_sigprocmask, "SIGPROCMASK" },
  [__NR_rt_sigreturn]   = { elkvm_do_sigreturn, "SIGRETURN" },
  [__NR_ioctl]          = { elkvm_do_ioctl, "IOCTL" },
  [__NR_pread64]        = { elkvm_do_pread64, "PREAD64" },
  [__NR_pwrite64]       = { elkvm_do_pwrite64, "PWRITE64" },
  [__NR_readv]          = { elkvm_do_readv, "READV" },
  [__NR_writev]         = { elkvm_do_writev, "WRITEV" },
  [__NR_access]         = { elkvm_do_access, "ACCESS" },
  [__NR_pipe]           = { elkvm_do_pipe, "PIPE" },
  [__NR_select]         = { elkvm_do_select, "SELECT" },
  [__NR_sched_yield]    = { elkvm_do_sched_yield, "SCHED YIELD" },
  [__NR_mremap]         = { elkvm_do_mremap, "MREMAP" },
  [__NR_msync]          = { elkvm_do_msync, "MSYNC" },
  [__NR_mincore]        = { elkvm_do_mincore, "MINCORE" },
  [__NR_madvise]        = { elkvm_do_madvise, "MADVISE" },
  [__NR_shmget]         = { elkvm_do_shmget, "SHMGET" },
  [__NR_shmat]          = { elkvm_do_shmat, "SHMAT" },
  [__NR_shmctl]         = { elkvm_do_shmctl, "SHMCTL" },
  [__NR_dup]            = { elkvm_do_dup, "DUP" },
  [__NR_dup2]           = { elkvm_do_dup2, "DUP2" },
  [__NR_pause]          = { elkvm_do_pause, "PAUSE" },
  [__NR_nanosleep]      = { elkvm_do_nanosleep, "NANOSLEEP" },
  [__NR_getitimer]      = { elkvm_do_getitimer, "GETITIMER" },
  [__NR_alarm]          = { elkvm_do_alarm, "ALARM" },
  [__NR_setitimer]      = { elkvm_do_setitimer, "SETITIMER" },
  [__NR_getpid]         = { elkvm_do_getpid, "GETPID" },
  [__NR_sendfile]       = { elkvm_do_sendfile, "SENDFILE" },
  [__NR_socket]         = { elkvm_do_socket, "SOCKET" },
  [__NR_connect]        = { elkvm_do_connect, "CONNECT" },
  [__NR_accept]         = { elkvm_do_accept, "ACCEPT" },
  [__NR_sendto]         = { elkvm_do_sendto, "SENDTO" },
  [__NR_recvfrom]       = { elkvm_do_recvfrom, "RECVFROM" },
  [__NR_sendmsg]        = { elkvm_do_sendmsg, "SENDMSG" },
  [__NR_recvmsg]        = { elkvm_do_recvmsg, "RECVMSG" },
  [__NR_shutdown]       = { elkvm_do_shutdown, "SHUTDOWN" },
  [__NR_bind]           = { elkvm_do_bind, "BIND" },
  [__NR_listen]         = { elkvm_do_listen, "LISTEN" },
  [__NR_getsockname]    = { elkvm_do_getsockname, "GETSOCKNAME" },
  [__NR_getpeername]    = { elkvm_do_getpeername, "GETPEERNAME" },
  [__NR_socketpair]     = { elkvm_do_socketpair, "SOCKETPAIR" },
  [__NR_setsockopt]     = { elkvm_do_setsockopt, "SETSOCKOPT" },
  [__NR_getsockopt]     = { elkvm_do_getsockopt, "GETSOCKOPT" },
  [__NR_clone]          = { elkvm_do_clone, "CLONE" },
  [__NR_fork]           = { elkvm_do_fork, "FORK" },
  [__NR_vfork]          = { elkvm_do_vfork, "VFORK" },
  [__NR_execve]         = { elkvm_do_execve, "EXECVE" },
  [__NR_exit]           = { elkvm_do_exit, "EXIT" },
  [__NR_wait4]          = { elkvm_do_wait4, "WAIT4" },
  [__NR_kill]           = { elkvm_do_kill, "KILL" },
  [__NR_uname]          = { elkvm_do_uname, "UNAME" },
  [__NR_semget]         = { elkvm_do_semget, "SEMGET" },
  [__NR_semop]          = { elkvm_do_semop, "SEMOP" },
  [__NR_semctl]         = { elkvm_do_semctl, "SEMCTL" },
  [__NR_shmdt]          = { elkvm_do_shmdt, "SHMDT" },
  [__NR_msgget]         = { elkvm_do_msgget, "MSGGET" },
  [__NR_msgsnd]         = { elkvm_do_msgsnd, "MSGSND" },
  [__NR_msgrcv]         = { elkvm_do_msgrcv, "MSGRCV" },
  [__NR_msgctl]         = { elkvm_do_msgctl, "MSGCTL" },
  [__NR_fcntl]          = { elkvm_do_fcntl, "FCNTL" },
  [__NR_flock]          = { elkvm_do_flock, "FLOCK" },
  [__NR_fsync]          = { elkvm_do_fsync, "FSYNC" },
  [__NR_fdatasync]      = { elkvm_do_fdatasync, "FDATASYNC" },
  [__NR_truncate]       = { elkvm_do_truncate, "TRUNCATE" },
  [__NR_ftruncate]      = { elkvm_do_ftruncate, "FTRUNCATE" },
  [__NR_getdents]       = { elkvm_do_getdents, "GETDENTS" },
  [__NR_getcwd]         = { elkvm_do_getcwd, "GETCWD" },
  [__NR_chdir]          = { elkvm_do_chdir, "CHDIR" },
  [__NR_fchdir]         = { elkvm_do_fchdir, "FCHDIR" },
  [__NR_rename]         = { elkvm_do_rename, "RENAME" },
  [__NR_mkdir]          = { elkvm_do_mkdir, "MKDIR" },
  [__NR_rmdir]          = { elkvm_do_rmdir, "RMDIR" },
  [__NR_creat]          = { elkvm_do_creat, "CREAT" },
  [__NR_link]           = { elkvm_do_link, "LINK" },
  [__NR_unlink]         = { elkvm_do_unlink, "UNLINK" },
  [__NR_symlink]        = { elkvm_do_symlink, "SYMLINK" },
  [__NR_readlink]       = { elkvm_do_readlink, "READLINK" },
  [__NR_chmod]          = { elkvm_do_chmod, "CHMOD" },
  [__NR_fchmod]         = { elkvm_do_fchmod, "FCHMOD" },
  [__NR_chown]          = { elkvm_do_chown, "CHOWN" },
  [__NR_fchown]         = { elkvm_do_fchown, "FCHOWN" },
  [__NR_lchown]         = { elkvm_do_lchown, "LCHOWN" },
  [__NR_umask]          = { elkvm_do_umask, "UMASK" },
  [__NR_gettimeofday]   = { elkvm_do_gettimeofday, "GETTIMEOFDAY" },
  [__NR_getrlimit]      = { elkvm_do_getrlimit , "GETRLIMIT" },
  [__NR_getrusage]      = { elkvm_do_getrusage, "GETRUSAGE" },
  [__NR_sysinfo]        = { elkvm_do_sysinfo, "SYSINFO" },
  [__NR_times]          = { elkvm_do_times, "TIMES" },
  [__NR_ptrace]         = { elkvm_do_ptrace, "PTRACE" },
  [__NR_getuid]         = { elkvm_do_getuid, "GETUID" },
  [__NR_syslog]         = { elkvm_do_syslog, "SYSLOG" },
  [__NR_getgid]         = { elkvm_do_getgid, "GETGID" },
  [__NR_setuid]         = { elkvm_do_setuid, "SETUID" },
  [__NR_setgid]         = { elkvm_do_setgid, "SETGID" },
  [__NR_geteuid]        = { elkvm_do_geteuid, "GETEUID" },
  [__NR_getegid]        = { elkvm_do_getegid, "GETEGID" },
  [__NR_setpgid]        = { elkvm_do_setpgid, "GETPGID" },
  [__NR_getppid]        = { elkvm_do_getppid, "GETPPID" },
  [__NR_getpgrp]        = { elkvm_do_getpgrp, "GETPGRP" },
  [__NR_setsid]         = { elkvm_do_setsid, "SETSID" },
  [__NR_setreuid]       = { elkvm_do_setreuid, "SETREUID" },
  [__NR_setregid]       = { elkvm_do_setregid, "SETREGID" },
  [__NR_getgroups]      = { elkvm_do_getgroups, "GETGROUPS" },
  [__NR_setgroups]      = { elkvm_do_setgroups, "SETGROUPS" },
  [__NR_setresuid]      = { elkvm_do_setresuid, "SETRESUID" },
  [__NR_getresuid]      = { elkvm_do_getresuid, "GETRESUID" },
  [__NR_setresgid]      = { elkvm_do_setresgid, "SETRESGID" },
  [__NR_getresgid]      = { elkvm_do_getresgid, "GETRESGID" },
  [__NR_getpgid]        = { elkvm_do_getpgid, "GETPGID" },
  [__NR_setfsuid]       = { elkvm_do_setfsuid, "SETFSUID" },
  [__NR_setfsgid]       = { elkvm_do_setfsgid, "SETFSGID" },
  [__NR_getsid]         = { elkvm_do_getsid, "GETSID" },
  [__NR_capget]         = { elkvm_do_capget, "CAPGET" },
  [__NR_capset]         = { elkvm_do_capset, "CAPSET" },
  [__NR_rt_sigpending]  = { elkvm_do_rt_sigpending, "RT SIGPENDING" },
  [__NR_rt_sigtimedwait] = { elkvm_do_rt_sigtimedwait, "RT SIGTIMEDWAIT" },
  [__NR_rt_sigqueueinfo] = { elkvm_do_rt_sigqueueinfo, "RT SIGQUEUEINFO" },
  [__NR_rt_sigsuspend]   = { elkvm_do_rt_sigsuspend, "RT SIGSUSPEND" },
  [__NR_sigaltstack]     = { elkvm_do_sigaltstack, "SIGALTSTACK" },
  [__NR_utime]           = { elkvm_do_utime, "UTIME" },
  [__NR_mknod]           = { elkvm_do_mknod, "MKNOD" },
  [__NR_uselib]          = { elkvm_do_uselib, "USELIB" },
  [__NR_personality]     = { elkvm_do_personality, "PERSONALITY" },
  [__NR_ustat]           = { elkvm_do_ustat, "USTAT" },
  [__NR_statfs]          = { elkvm_do_statfs, "STATFS" },
  [__NR_fstatfs]         = { elkvm_do_fstatfs, "FSTATFS" },
  [__NR_sysfs]           = { elkvm_do_sysfs, "SYSFS" },
  [__NR_getpriority]     = { elkvm_do_getpriority, "GETPRIORITY" },
  [__NR_setpriority]     = { elkvm_do_setpriority, "SETPRIORITY" },
  [__NR_sched_setparam]  = { elkvm_do_sched_setparam, "SCHED SETPARAM" },
  [__NR_sched_getparam]  = { elkvm_do_sched_getparam, "SCHED GETPARAM" },
  [__NR_sched_setscheduler] = { elkvm_do_sched_setscheduler, "SCHED SETSCHEDULER" },
  [__NR_sched_getscheduler] = { elkvm_do_sched_getscheduler, "SCHED GETSCHEDULER" },
  [__NR_sched_get_priority_max] = { elkvm_do_sched_get_priority_max, "SCHED GET PRIORITY MAX" },
  [__NR_sched_get_priority_min] = { elkvm_do_sched_get_priority_min, "SCHED GET PRIORITY MIN" },
  [__NR_sched_rr_get_interval]  = { elkvm_do_sched_rr_get_interval, "SCHED RR GET INTERVAL" },
  [__NR_mlock]                  = { elkvm_do_mlock, "MLOCK" },
  [__NR_munlock]                = { elkvm_do_munlock, "MUNLOCK" },
  [__NR_mlockall]               = { elkvm_do_mlockall, "MLOCKALL" },
  [__NR_munlockall]             = { elkvm_do_munlockall, "MUNLOCKALL" },
  [__NR_vhangup]                = { elkvm_do_vhangup, "VHANGUP" },
  [__NR_modify_ldt]             = { elkvm_do_modify_ldt, "MODIFY LDT" },
  [__NR_pivot_root]             = { elkvm_do_pivot_root, "PIVOT ROOT" },
  [__NR__sysctl]                = { elkvm_do_sysctl, " SYSCTL" },
  [__NR_prctl]                  = { elkvm_do_prctl, "PRCTL" },
  [__NR_arch_prctl]             = { elkvm_do_arch_prctl, "ARCH PRCTL" },
  [__NR_adjtimex]               = { elkvm_do_adjtimex, "ADJTIMEX" },
  [__NR_setrlimit]              = { elkvm_do_setrlimit, "SETRLIMIT" },
  [__NR_chroot]                 = { elkvm_do_chroot, "CHROOT" },
  [__NR_sync]                   = { elkvm_do_sync, "SYNC" },
  [__NR_acct]                   = { elkvm_do_acct, "ACCT" },
  [__NR_settimeofday]           = { elkvm_do_settimeofday, "SETTIMEOFDAY" },
  [__NR_mount]                  = { elkvm_do_mount, "MOUNT" },
  [__NR_umount2]                = { elkvm_do_umount2, "UMOUNT2" },
  [__NR_swapon]                 = { elkvm_do_swapon, "SWAPON" },
  [__NR_swapoff]                = { elkvm_do_swapoff, "SWAPOFF" },
  [__NR_reboot]                 = { elkvm_do_reboot, "REBOOT" },
  [__NR_sethostname]            = { elkvm_do_sethostname, "SETHOSTNAME" },
  [__NR_setdomainname]          = { elkvm_do_setdomainname, "SETDOMAINNAME" },
  [__NR_iopl]                   = { elkvm_do_iopl, "IOPL" },
  [__NR_ioperm]                 = { elkvm_do_ioperm, "IOPERM" },
  [__NR_create_module]          = { elkvm_do_create_module, "CREATE MODULE" },
  [__NR_init_module]            = { elkvm_do_init_module, "INIT MODULE" },
  [__NR_delete_module]          = { elkvm_do_delete_module, "DELETE MODULE" },
  [__NR_get_kernel_syms]        = { elkvm_do_get_kernel_syms, "GET KERNEL SYMS" },
  [__NR_query_module]           = { elkvm_do_query_module, "QUERY MODULE" },
  [__NR_quotactl]               = { elkvm_do_quotactl, "QUOTACTL" },
  [__NR_nfsservctl]             = { elkvm_do_nfsservctl, "NFSSERVCTL" },
  [__NR_getpmsg]                = { elkvm_do_getpmsg, "GETPMSG" },
  [__NR_putpmsg]                = { elkvm_do_putpmsg, "PUTPMSG" },
  [__NR_afs_syscall]            = { elkvm_do_afs_syscall, "AFS SYSCALL" },
  [__NR_tuxcall]                = { elkvm_do_tuxcall, "TUXCALL" },
  [__NR_security]               = { elkvm_do_security, "SECURITY" },
  [__NR_gettid]                 = { elkvm_do_gettid, "GETTID" },
  [__NR_readahead]              = { elkvm_do_readahead, "READAHEAD" },
  [__NR_setxattr]               = { elkvm_do_setxattr, "SETXATTR" },
  [__NR_lsetxattr]              = { elkvm_do_lsetxattr, "LETSETXATTR" },
  [__NR_fsetxattr]              = { elkvm_do_fsetxattr, "FSETXATTR" },
  [__NR_getxattr]               = { elkvm_do_getxattr, "GETXATTR" },
  [__NR_lgetxattr]              = { elkvm_do_lgetxattr, "LGETXATTR" },
  [__NR_fgetxattr]              = { elkvm_do_fgetxattr, "FGETXATTR" },
  [__NR_listxattr]              = { elkvm_do_listxattr, "LISTXATTR" },
  [__NR_llistxattr]             = { elkvm_do_llistxattr, "LLISTXATTR" },
  [__NR_flistxattr]             = { elkvm_do_flistxattr, "FLISTXATTR" },
  [__NR_removexattr]            = { elkvm_do_removexattr, "REMOVEXATTR" },
  [__NR_lremovexattr]           = { elkvm_do_lremovexattr, "LREMOVEXATTR" },
  [__NR_fremovexattr]           = { elkvm_do_fremovexattr, "FREMOVEXATTR" },
  [__NR_tkill]                  = { elkvm_do_tkill, "TKILL" },
  [__NR_time]                   = { elkvm_do_time, "TIME" },
  [__NR_futex]                  = { elkvm_do_futex, "FUTEX" },
  [__NR_sched_setaffinity]      = { elkvm_do_sched_setaffinity, "SCHED SETAFFINITY" },
  [__NR_sched_getaffinity]      = { elkvm_do_sched_getaffinity, "SCHED GETAFFINITY" },
  [__NR_set_thread_area]        = { elkvm_do_set_thread_area, "SET THREAD AREA" },
  [__NR_io_setup]               = { elkvm_do_io_setup, "IO SETUP" },
  [__NR_io_destroy]             = { elkvm_do_io_destroy, "IO DESTROY" },
  [__NR_io_getevents]           = { elkvm_do_getevents, "IO GETEVENTS" },
  [__NR_io_submit]              = { elkvm_do_submit, "IO SUBMIT" },
  [__NR_io_cancel]              = { elkvm_do_cancel, "IO CANCEL" },
  [__NR_get_thread_area]        = { elkvm_do_get_thread_area, "GET THREAD AREA" },
  [__NR_lookup_dcookie]         = { elkvm_do_lookup_dcookie, "LOOKUP DCOOKIE" },
  [__NR_epoll_create]           = { elkvm_do_epoll_create, "EPOLL CREATE" },
  [__NR_epoll_ctl_old]          = { elkvm_do_epoll_ctl_old, "EPOLL CTL OLD" },
  [__NR_epoll_wait_old]         = { elkvm_do_epoll_wait_old, "EPOLL WAIT OLD" },
  [__NR_remap_file_pages]       = { elkvm_do_remap_file_pages, "REMAP FILE PAGES" },
  [__NR_getdents64]             = { elkvm_do_getdents64, "GETDENTS64" },
  [__NR_set_tid_address]        = { elkvm_do_set_tid_address, "SET TID ADDRESS" },
  [__NR_restart_syscall]        = { elkvm_do_restart_syscall, "RESTART SYSCALL" },
  [__NR_semtimedop]             = { elkvm_do_semtimedop, "SEMTIMEDOP" },
  [__NR_fadvise64]              = { elkvm_do_fadive64, "FADVISE64" },
  [__NR_timer_create]           = { elkvm_do_timer_create, "TIMER CREATE" },
  [__NR_timer_settime]          = { elkvm_do_timer_settime, "TIMER SETTIME" },
  [__NR_timer_gettime]          = { elkvm_do_timer_gettime, "TIMER GETTIME" },
  [__NR_timer_getoverrun]       = { elkvm_do_timer_getoverrun, "TIMER GETOVERRUN" },
  [__NR_timer_delete]           = { elkvm_do_timer_delete, "TIMER DELETE" },
  [__NR_clock_settime]   = { elkvm_do_clock_settime, "CLOCK SETTIME" },
  [__NR_clock_gettime]   = { elkvm_do_clock_gettime, "CLOCK GETTIME" },
  [__NR_clock_getres]    = { elkvm_do_clock_getres, "CLOCK GETRES" },
  [__NR_clock_nanosleep] = { elkvm_do_clock_nanosleep, "CLOCK NANOSLEEP" },
  [__NR_exit_group]      = { elkvm_do_exit_group, "EXIT GROUP" },
  [__NR_epoll_wait]      = { elkvm_do_epoll_wait, "EPOLL WAIT" },
  [__NR_epoll_ctl]       = { elkvm_do_epoll_ctl, "EPOLL CTL" },
  [__NR_tgkill]          = { elkvm_do_tgkill, "TGKILL" },
  [__NR_utimes]          = { elkvm_do_utimes, "UTIMES" },
  [__NR_vserver]         = { elkvm_do_vserver, "VSERVER" },
  [__NR_mbind]           = { elkvm_do_mbind, "MBIND" },
  [__NR_set_mempolicy]   = { elkvm_do_mpolicy, "SET MPOLICY" },
  [__NR_get_mempolicy]   = { elkvm_do_get_mempolicy, "GET MEMPOLICY" },
  [__NR_mq_open]         = { elkvm_do_mq_open, "MQ OPEN" },
  [__NR_mq_unlink]       = { elkvm_do_mq_unlink, "MQ UNLINK" },
  [__NR_mq_timedsend]    = { elkvm_do_mq_timedsend, "MQ TIMEDSEND" },
  [__NR_mq_timedreceive] = { elkvm_do_mq_timedreceive, "MQ TIMEDRECEIVE" },
  [__NR_mq_notify]       = { elkvm_do_mq_notify, "MQ NOTIFY" },
  [__NR_mq_getsetattr]   = { elkvm_do_getsetattr, "MQ GETSETATTR" },
  [__NR_kexec_load]      = { elkvm_do_kexec_load, "KEXEC LOAD" },
  [__NR_waitid]          = { elkvm_do_waitid, "WAITID" },
  [__NR_add_key]         = { elkvm_do_add_key, "ADD KEY" },
  [__NR_request_key]     = { elkvm_do_request_key, "REQUEST KEY" },
  [__NR_keyctl]          = { elkvm_do_keyctl, "KEYCTL" },
  [__NR_ioprio_set]      = { elkvm_do_ioprio_set, "IOPRIO SET" },
  [__NR_ioprio_get]      = { elkvm_do_ioprio_get, "IOPRIO GET" },
  [__NR_inotify_init]    = { elkvm_do_inotify_init, "INOTIFY INIT" },
  [__NR_inotify_add_watch] = { elkvm_do_inotify_add_watch, "INOTIFY ADD WATCH" },
  [__NR_inotify_rm_watch]  = { elkvm_do_inotify_rm_watch, "INOTIFY RM WATCH" },
  [__NR_migrate_pages]     = { elkvm_do_migrate_pages, "MIGRATE PAGES" },
  [__NR_openat]          = { elkvm_do_openat, "OPENAT" },
  [__NR_mkdirat]         = { elkvm_do_mkdirat, "MKDIRAT" },
  [__NR_mknodat]         = { elkvm_do_mknodat, "MKNODAT" },
  [__NR_fchownat]        = { elkvm_do_fchownat, "FCHOWNAT" },
  [__NR_futimesat]       = { elkvm_do_futimesat, "FUTIMESAT" },
  [__NR_newfstatat]      = { elkvm_do_newfstatat, "NEWFSTATAT" },
  [__NR_unlinkat]        = { elkvm_do_unlinkat, "UNLINKAT" },
  [__NR_renameat]        = { elkvm_do_renameat, "RENAMEAT" },
  [__NR_linkat]          = { elkvm_do_linkat, "LINKAT" },
  [__NR_symlinkat]       = { elkvm_do_symlinkat, "SYMLINKAT" },
  [__NR_readlinkat]      = { elkvm_do_readlinkat, "READLINKAT" },
  [__NR_fchmodat]        = { elkvm_do_fchmodat, "FCHMODAT" },
  [__NR_faccessat]       = { elkvm_do_faccessat, "FACCESSAT" },
  [__NR_pselect6]        = { elkvm_do_pselect6, "PSELECT6" },
  [__NR_ppoll]           = { elkvm_do_ppoll, "PPOLL" },
  [__NR_unshare]         = { elkvm_do_unshare, "UNSHARE" },
  [__NR_set_robust_list] = { elkvm_do_set_robust_list, "SET ROBUST LIST" },
  [__NR_get_robust_list] = { elkvm_do_get_robust_list, "GET ROBUST LIST" },

};

int Elkvm::VM::handle_hypercall(const std::shared_ptr<Elkvm::VCPU>& vcpu) {

  int err = 0;

  uint64_t call = Elkvm::get_hypercall_type(vcpu);
  const Elkvm::hypercall_handlers *hyphandlers = get_hyp_handlers();

  if (hyphandlers->pre_handler) {
      hyphandlers->pre_handler(this, vcpu, call);
  }

  switch(call) {
    case ELKVM_HYPERCALL_SYSCALL:
      err = handle_syscall(vcpu);
      break;
    case ELKVM_HYPERCALL_INTERRUPT:
      err = handle_interrupt(vcpu);
      if(err) {
        return err;
      }
      break;
    default:
      ERROR() << "Hypercall was something else, don't know how to handle\n"
              << "Hypercall Num: " << std::dec << call << "\nABORT!\n";
      return 1;
  }

  if(err) {
    if(err != ELKVM_HYPERCALL_EXIT) {
      fprintf(stderr, "ELKVM: Could not handle hypercall!\n");
      fprintf(stderr, "Errno: %i Msg: %s\n", err, strerror(err));
    }
    return err;
  }

  if (hyphandlers->post_handler) {
      hyphandlers->post_handler(this, vcpu, call);
  }

  elkvm_emulate_vmcall(vcpu);

  err = signal_deliver();
  assert(err == 0);

  return 0;
}

int Elkvm::VM::handle_syscall(const std::shared_ptr<Elkvm::VCPU>& vcpu)
{
  CURRENT_ABI::paramtype syscall_num = CURRENT_ABI::get_parameter(vcpu, 0);
  if(debug_mode()) {
    DBG() << "SYSCALL " << std::dec << syscall_num << " detected"
      << " (" << elkvm_syscalls[syscall_num].name << ")";
  }

  long result;
  if(syscall_num > NUM_SYSCALLS) {
    ERROR() << "\tINVALID syscall_num: " << syscall_num << "\n";
    result = -ENOSYS;
  } else {
    result = elkvm_syscalls[syscall_num].func(this);
    if(syscall_num == __NR_exit_group) {
      return ELKVM_HYPERCALL_EXIT;
    }
  }
  /* binary expects syscall result in rax */
  CURRENT_ABI::set_syscall_return(vcpu, result);

  return 0;
}

namespace Elkvm {

void dbg_log_read(const Elkvm::VM &vm, const int fd, const guestptr_t buf_p,
    const void *buf, const size_t parcount, const size_t count,
    const size_t result) {
  if(vm.debug_mode()) {
    DBG() << "READ from fd: " << fd
          << " with size " << LOG_DEC_HEX(parcount) << " of "
          << LOG_DEC_HEX(count)
          << " buf @ " << LOG_GUEST_HOST(buf_p, buf);
    dbg_log_result(result);
  }
}

  //namespace Elkvm
}

long elkvm_do_read(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->read == NULL) {
    ERROR() << "READ handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  CURRENT_ABI::paramtype fd;
  CURRENT_ABI::paramtype buf_p = 0x0;
  char *buf;
  CURRENT_ABI::paramtype count;

  vmi->unpack_syscall(&fd, &buf_p, &count);

  assert(buf_p != 0x0);
  buf = reinterpret_cast<char *>(vmi->get_region_manager()->get_pager().get_host_p(buf_p));

  uint64_t bend_p = buf_p + count - 1;
  void *bend = vmi->get_region_manager()->get_pager().get_host_p(bend_p);
  long result = 0;

  if(!vmi->get_region_manager()->same_region(buf, bend)) {
    assert(vmi->get_region_manager()->host_address_mapped(bend));
    char *host_begin_mark = NULL;
    char *host_end_mark = buf;
    uint64_t mark_p = buf_p;
    ssize_t current_count = count;
    do {
      host_begin_mark = reinterpret_cast<char *>(vmi->get_region_manager()->get_pager().get_host_p(mark_p));
      std::shared_ptr<Elkvm::Region> region = vmi->get_region_manager()->find_region(host_begin_mark);
      if(mark_p != buf_p) {
        assert(host_begin_mark == region->base_address());
      }

      host_end_mark = reinterpret_cast<char *>(region->last_valid_address());
      assert(host_end_mark > host_begin_mark);

      ssize_t newcount = host_end_mark - host_begin_mark;
      if(newcount > current_count) {
        newcount = current_count;
      }

      long in_result = vmi->get_handlers()->read((int)fd, host_begin_mark, newcount);
      if(in_result < 0) {
        return errno;
      }
      if(in_result < newcount) {
        return result + in_result;
      }
      Elkvm::dbg_log_read(*vmi, fd, buf_p, buf, newcount, count, result);

      mark_p += in_result;
      current_count -= in_result;
      result += in_result;
    } while(!vmi->get_region_manager()->same_region(host_begin_mark, bend));
    assert(current_count == 0);

  } else {
    result = vmi->get_handlers()->read((int)fd, buf, (size_t)count);
  }

  Elkvm::dbg_log_read(*vmi, fd, buf_p, buf, count, count, result);

  return result;
}

long elkvm_do_write(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->write == NULL) {
    ERROR() << "WRITE handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  CURRENT_ABI::paramtype fd = 0x0;
  guestptr_t buf_p = 0x0;
  void *buf;
  CURRENT_ABI::paramtype count = 0x0;

  vmi->unpack_syscall(&fd, &buf_p, &count);

  assert(buf_p != 0x0);
  buf = vmi->get_region_manager()->get_pager().get_host_p(buf_p);

  std::shared_ptr<Elkvm::Region> r = vmi->get_region_manager()->find_region(buf);
  assert(r != nullptr);

  char *current_buf = reinterpret_cast<char *>(buf);
  size_t remaining_count = count;
  ssize_t total = 0;
  while(!r->contains_address(current_buf + remaining_count - 1)) {
    long result = vmi->get_handlers()->write(static_cast<int>(fd),
        current_buf, r->space_after_address(current_buf));
    if(result < 0) {
      return -errno;
    }
    total += result;

    if(vmi->debug_mode()) {
      DBG() << "SPLIT WRITE to fd: " << fd << " with size " << count
            << " buf " << (void*)buf_p << "(" << (void*)buf << ")" << LOG_RESET;
      DBG() << "current buf: " << (void*)current_buf
            << " remaining bytes: " << remaining_count << LOG_RESET;
      DBG() << "RESULT " <<  result;
    }
    current_buf += result;
    remaining_count -= result;
    r = vmi->get_region_manager()->find_region(current_buf);
  }
  assert(r->contains_address(reinterpret_cast<char *>(buf) + count - 1));

  long result = vmi->get_handlers()->write(static_cast<int>(fd),
      current_buf, remaining_count);
  if(result < 0) {
    return -errno;
  }
  total += result;

  if(vmi->debug_mode()) {
    DBG() << "SPLIT WRITE to fd: " << fd << " with size " << count
          << " buf " << (void*)buf_p << "(" << (void*)buf << ")" << LOG_RESET;
    DBG() << "RESULT " <<  result;
  }

  return total;
}

long elkvm_do_close(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->close == NULL) {
    ERROR () << "CLOSE handler not found\n";
    return -ENOSYS;
  }

  CURRENT_ABI::paramtype fd = 0;
  vmi->unpack_syscall(&fd);

  long result = vmi->get_handlers()->close((int)fd);

  if(vmi->debug_mode()) {
    DBG() << "CLOSE file with fd: " << fd;
    Elkvm::dbg_log_result<int>(result);
  }

  return result;
}

long elkvm_do_stat(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->stat == NULL) {
    ERROR() << "STAT handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  CURRENT_ABI::paramtype path_p = 0;
  CURRENT_ABI::paramtype buf_p = 0;
  char *path = NULL;
  struct stat *buf;
  vmi->unpack_syscall(&path_p, &buf_p);

  assert(path_p != 0x0);
  path = reinterpret_cast<char *>(vmi->get_region_manager()->get_pager().get_host_p(path_p));
  assert(buf_p != 0x0);
  buf  = reinterpret_cast<struct stat *>(vmi->get_region_manager()->get_pager().get_host_p(buf_p));

  long result = vmi->get_handlers()->stat(path, buf);
  if(vmi->debug_mode()) {
    DBG() << "STAT file " << path << " with buf at: " << (void*)buf_p << "(" << (void*)buf << ")";
    Elkvm::dbg_log_result<int>(result);
  }

  return result;
}

long elkvm_do_fstat(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->fstat == NULL) {
    ERROR() << "FSTAT handler not found\n";
    return -ENOSYS;
  }

  CURRENT_ABI::paramtype fd = 0;
  CURRENT_ABI::paramtype buf_p = 0;
  struct stat *buf = NULL;
  vmi->unpack_syscall(&fd, &buf_p);

  assert(buf_p != 0x0);
  buf = reinterpret_cast<struct stat *>(vmi->get_region_manager()->get_pager().get_host_p(buf_p));

  if(vmi->debug_mode()) {
    DBG() << "FSTAT file with fd " << fd << " buf at " << LOG_GUEST_HOST(buf_p, buf);
  }
  long result = vmi->get_handlers()->fstat(fd, buf);

  if(vmi->debug_mode()) {
    Elkvm::dbg_log_result<int>(result);
  }

  return result;
}

long elkvm_do_lstat(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->lstat == NULL) {
    ERROR() << "LSTAT handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  CURRENT_ABI::paramtype path_p = 0;
  CURRENT_ABI::paramtype buf_p = 0;
  char *path = NULL;
  struct stat *buf;
  vmi->unpack_syscall(&path_p, &buf_p);

  assert(path_p != 0x0);
  path = reinterpret_cast<char *>(vmi->get_region_manager()->get_pager().get_host_p(path_p));
  assert(buf_p != 0x0);
  buf  = reinterpret_cast<struct stat *>(vmi->get_region_manager()->get_pager().get_host_p(buf_p));

  long result = vmi->get_handlers()->lstat(path, buf);
  if(vmi->debug_mode()) {
    DBG() << "LSTAT file " << path << " with buf at " << (void*) buf_p
          << " (" << (void*)buf << ")";
    Elkvm::dbg_log_result<int>(result);
  }

  return result;
}

long elkvm_do_lseek(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->lseek == NULL) {
    ERROR() << "LSEEK handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  CURRENT_ABI::paramtype fd;
  CURRENT_ABI::paramtype off;
  CURRENT_ABI::paramtype whence;

  vmi->unpack_syscall(&fd, &off, &whence);

  long result = vmi->get_handlers()->lseek(fd, off, whence);
  if(vmi->debug_mode()) {
    DBG() << "LSEEK fd " << fd << " offset " << off << " whence " << whence;
    Elkvm::dbg_log_result<int>(result);

  }
  return result;


}

static inline void
log_mmap_args(guestptr_t addr, CURRENT_ABI::paramtype length,
              CURRENT_ABI::paramtype prot, CURRENT_ABI::paramtype flags,
              CURRENT_ABI::paramtype fd, CURRENT_ABI::paramtype off)
{
  DBG() << LOG_CYAN << "address " << std::hex << addr
        << " length " << length << LOG_RESET;

  DBG () << LOG_CYAN << "protection: (" << std::hex << prot << ") "
         << (prot & PROT_NONE ? "PROT_NONE " : "")
         << (prot & PROT_WRITE ? "PROT_WRITE " : "")
         << (prot & PROT_READ ? "PROT_READ " : "")
         << (prot & PROT_EXEC ? "PROT_EXEC " : "") << LOG_RESET;

  DBG () << LOG_CYAN << "flags: (" << std::hex << flags << ") "
         << (flags & MAP_SHARED ? "MAP_SHARED " : "")
         << (flags & MAP_PRIVATE ? "MAP_PRIVATE " : "")
         << (flags & MAP_ANONYMOUS ? "MAP_ANONYMOUS " : "")
         << (flags & MAP_32BIT ? "MAP_32BIT " : "")
         << (flags & MAP_DENYWRITE ? "MAP_DENYWRITE " : "")
         << (flags & MAP_FIXED ? "MAP_FIXED " : "")
         << (flags & MAP_GROWSDOWN ? "MAP_GROWSDOWN " : "")
         << (flags & MAP_LOCKED ? "MAP_LOCKED " : "")
         << (flags & MAP_NONBLOCK ? "MAP_NONBLOCK " : "")
         << (flags & MAP_NORESERVE ? "MAP_NORESERVE " : "")
         << (flags & MAP_POPULATE ? "MAP_POPULATE " : "")
         << (flags & MAP_STACK ? "MAP_STACK " : "")
         << LOG_RESET;

  DBG() << LOG_CYAN << "fd " << fd << " offset " << off << LOG_RESET;
}

#define ELKVM_DEBUG_MMAP 0

long elkvm_do_mmap(Elkvm::VM * vmi) {
  /* obtain a region_mapping and fill this with a proposal
   * on how to do the mapping */
  guestptr_t addr               = 0x0;
  CURRENT_ABI::paramtype length = 0x0;
  CURRENT_ABI::paramtype prot   = 0x0;
  CURRENT_ABI::paramtype flags  = 0x0;
  CURRENT_ABI::paramtype fd     = 0;
  CURRENT_ABI::paramtype off    = 0;

  vmi->unpack_syscall(&addr, &length, &prot, &flags, &fd, &off);
#if ELKVM_DEBUG_MMAP
  log_mmap_args(addr, length, prot, flags, fd, off);
  vmi->get_region_manager()->dump_regions();
#endif

  /* create a mapping object with the data from the user, this will
   * also allocate the memory for this mapping */
  Elkvm::Mapping &mapping =
    vmi->get_heap_manager().get_mapping(addr, length, prot, flags, fd, off);


  /* if a handler is specified, call the monitor for corrections etc. */
  long result = 0;
  if(vmi->get_handlers()->mmap_before != NULL) {
    struct region_mapping *cm = mapping.c_mapping();
    result = vmi->get_handlers()->mmap_before(cm);
    /* write changes back to mapping obj */
    const int remap = mapping.diff(cm);
    if(remap) {
      int err = vmi->get_heap_manager().unmap(mapping);
      assert(err == 0 && "could not unmap mapping");
    }
    mapping.sync_back(cm);
    if(remap) {
      int err = vmi->get_heap_manager().map(mapping);
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
  if(vmi->get_handlers()->mmap_after != NULL) {
    result = vmi->get_handlers()->mmap_after(mapping.c_mapping());
  }

  if(vmi->debug_mode()) {
    DBG() << "MMAP addr " << (void*)addr
        << " len " << LOG_DEC_HEX(length)
        << " prot " << prot << " flags " << flags << " ";
    if(!(flags & MAP_ANONYMOUS)) {
      DBG() << " -> fd " << std::dec << fd << " offs 0x" << std::hex << off;
    }
    if(flags & MAP_FIXED) {
      DBG() << " -> MAP_FIXED ";
    }
    print(std::cout, mapping);

    Elkvm::dbg_log_result<int>(result);
  }
  if(result < 0) {
    return -errno;
  }

#if ELKVM_DEBUG_MMAP
  vmi->get_region_manager()->dump_regions();
#endif

  return mapping.guest_address();
}

long elkvm_do_munmap(Elkvm::VM * vmi) {

  guestptr_t addr_p = 0;
  void *addr = NULL;
  CURRENT_ABI::paramtype length = 0;
  vmi->unpack_syscall(&addr_p, &length);

  if(addr_p != 0x0) {
    addr = vmi->get_region_manager()->get_pager().get_host_p(addr_p);
  }

  auto &hm = vmi->get_heap_manager();
  Elkvm::Mapping &mapping = hm.find_mapping(addr);
  if(vmi->debug_mode()) {
    print(std::cout, mapping);
  }
  auto pages_left = hm.unmap(mapping, addr_p, pages_from_size(length));

  if(vmi->debug_mode()) {
    DBG() << "MUNMAP requested with address " << std::hex
      << LOG_GUEST_HOST(addr_p, addr)
      << " len: 0x" << length;
    if(pages_left > 0) {
      print(std::cout, mapping);
    } else {
      DBG() << "mapping was deleted";
    }
  }

  return 0;

}

long elkvm_do_brk(Elkvm::VM * vmi) {
  guestptr_t user_brk_req = 0;
  vmi->unpack_syscall(&user_brk_req);

  if(vmi->debug_mode()) {
    DBG() << "BRK requested with address: " << (void*)user_brk_req
          << " current brk " << (void*)vmi->get_heap_manager().get_brk();
  }

  /* if the requested brk address is 0 just return the current brk address */
  if(user_brk_req == 0) {
    return vmi->get_heap_manager().get_brk();
  }

  int err = vmi->get_heap_manager().brk(user_brk_req);
  if(vmi->debug_mode()) {
    DBG() << "BRK done: err: " << err << " (" << strerror(err) << ") "
          << "new brk @ " << (void*)vmi->get_heap_manager().get_brk();
  }
  if(err) {
    return err;
  }
  return vmi->get_heap_manager().get_brk();
}

long elkvm_do_ioctl(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->ioctl == NULL) {
    ERROR() << "IOCTL handler not found\n";
    return -ENOSYS;
  }

  static bool warned = false;
  if(!warned) {
    INFO();
    INFO() << "IOCTL IS ONLY SUPPORTED FOR THREE ARGS BY ELKVM RIGHT NOW!";
    INFO();
    warned = true;
  }

  CURRENT_ABI::paramtype fd;
  CURRENT_ABI::paramtype request;
  CURRENT_ABI::paramtype argp_p;

  vmi->unpack_syscall(&fd, &request, &argp_p);

  char *argp = static_cast<char *>(
      vmi->get_region_manager()->get_pager().get_host_p(argp_p));

  long result = vmi->get_handlers()->ioctl(fd, request, argp);

  if(vmi->debug_mode()) {
    DBG() << "IOCTL with fd: " << std::dec << fd
          << " request: " << request
          << " argp: " << LOG_GUEST_HOST(argp_p, argp);
    Elkvm::dbg_log_result<int>(result);
  }

  if(result < 0) {
    return -errno;
  }

  return result;
}

void elkvm_get_host_iov(Elkvm::VM * vmi,
    uint64_t iov_p, uint64_t iovcnt, struct iovec *host_iov) {
  struct iovec *guest_iov = NULL;
  assert(iov_p != 0x0);
  guest_iov = reinterpret_cast<struct iovec *>
    (vmi->get_region_manager()->get_pager().get_host_p(iov_p));

  for(unsigned i = 0; i < iovcnt; i++) {
    assert(guest_iov[i].iov_base != NULL);
    host_iov[i].iov_base = vmi->get_region_manager()->get_pager().get_host_p(
        reinterpret_cast<guestptr_t>(guest_iov[i].iov_base));
    host_iov[i].iov_len  = guest_iov[i].iov_len;
  }

}

long elkvm_do_readv(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->readv == NULL) {
    ERROR() << "READV handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  CURRENT_ABI::paramtype fd = 0;
  CURRENT_ABI::paramtype iov_p = 0;
  CURRENT_ABI::paramtype iovcnt = 0;

  vmi->unpack_syscall(&fd, &iov_p, &iovcnt);

  struct iovec host_iov[iovcnt];
  elkvm_get_host_iov(vmi, iov_p, iovcnt, host_iov);

  long result = vmi->get_handlers()->readv(fd, host_iov, iovcnt);
  if(vmi->debug_mode()) {
    DBG() << "READV with df " << fd << " (@ " << (void*)&fd
          << ") iov @ " << (void*)iov_p << " count: " << iovcnt;
    Elkvm::dbg_log_result<int>(result);
    if(result < 0) {
      ERROR() << "ERROR No: " << errno << " Msg: " << strerror(errno);
    }
  }
  return result;
}

long elkvm_do_writev(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->writev == NULL) {
    ERROR() << "WRITEV handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  CURRENT_ABI::paramtype fd = 0;
  CURRENT_ABI::paramtype iov_p = 0;
  CURRENT_ABI::paramtype iovcnt = 0;

  vmi->unpack_syscall(&fd, &iov_p, &iovcnt);

  struct iovec host_iov[iovcnt];
  elkvm_get_host_iov(vmi, iov_p, iovcnt, host_iov);

  long result = vmi->get_handlers()->writev(fd, host_iov, iovcnt);
  if(vmi->debug_mode()) {
    DBG() << "WRITEV with fd: " << fd << " iov @ " << (void*)iov_p
          << " iovcnt " << iovcnt;
    Elkvm::dbg_log_result<int>(result);
  }
  return result;
}

long elkvm_do_access(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->access == NULL) {
    ERROR() << "ACCESS handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  CURRENT_ABI::paramtype path_p;
  CURRENT_ABI::paramtype mode;

  vmi->unpack_syscall(&path_p, &mode);

  assert(path_p != 0x0);
  char *pathname = reinterpret_cast<char *>(vmi->get_region_manager()->get_pager().get_host_p(path_p));
  if(pathname == NULL) {
    return -EFAULT;
  }

  long result = vmi->get_handlers()->access(pathname, mode);
  if(vmi->debug_mode()) {
    DBG() << "ACCESS with pathname: " << pathname << " (" << (void*)path_p
          << ") mode " << mode;
    Elkvm::dbg_log_result<int>(result);
  }

  if(result) {
    return -errno;
  }

  return 0;
}

long elkvm_do_pipe(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->pipe == NULL) {
    ERROR() << "PIPE handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  CURRENT_ABI::paramtype pipefd_p = 0x0;
  int *pipefd = NULL;

  vmi->unpack_syscall(&pipefd_p);

  pipefd = reinterpret_cast<int *>(vmi->get_region_manager()->get_pager().get_host_p(pipefd_p));
  assert(pipefd != NULL);

  long result = vmi->get_handlers()->pipe(pipefd);
  if(vmi->debug_mode()) {
    DBG() << "PIPE with pipefds at: " << pipefd << std::hex << "(" << pipefd << ")" << std::dec;
    Elkvm::dbg_log_result<int>(result);
  }
  if(result) {
    return -errno;
  }

  return 0;
}

long elkvm_do_mremap(Elkvm::VM *vmi) {
  guestptr_t old_address_p = 0x0;
  void *old_address = NULL;
  CURRENT_ABI::paramtype old_size = 0;
  CURRENT_ABI::paramtype new_size = 0;
  CURRENT_ABI::paramtype flags = 0;
  guestptr_t new_address_p = 0x0;
  void *new_address = NULL;

  vmi->unpack_syscall(&old_address_p, &old_size, &new_size, &flags, &new_address_p);

  if(old_address_p != 0x0) {
    old_address = vmi->get_region_manager()->get_pager().get_host_p(old_address_p);
  }
  if(new_address_p != 0x0) {
    new_address = vmi->get_region_manager()->get_pager().get_host_p(new_address_p);
  }

  Elkvm::Mapping &mapping = vmi->get_heap_manager().find_mapping(old_address);
  if(vmi->debug_mode()) {
    INFO() <<"MREMAP reguested with old address: 0x"
      << std::hex << old_address_p << " (" << old_address <<") size: 0x"
      << old_size << std::endl;
    INFO() <<"       ";
    if(flags & MREMAP_FIXED) {
      INFO() <<"new address: 0x"
      << new_address_p << " (" << new_address << ") ";
    }
    INFO() <<"size: 0x" << new_size
      << " flags:";
    INFO() <<((flags & MREMAP_MAYMOVE) ? " MREMAP_MAYMOVE" : "");
    INFO() <<((flags & MREMAP_FIXED)   ? " MREMAP_FIXED"   : "");
    INFO() <<std::endl;

    print(std::cout, mapping);
  }

  return vmi->get_heap_manager().remap(mapping, new_address_p, new_size, flags);
}

long elkvm_do_dup(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->dup == NULL) {
    ERROR() << "DUP handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }
  CURRENT_ABI::paramtype oldfd;

  vmi->unpack_syscall(&oldfd);

  if(vmi->debug_mode()) {
    DBG() << "CALLING DUP handler with oldfd " << oldfd << "\n";
  }

  long result = vmi->get_handlers()->dup(oldfd);
  if(vmi->debug_mode()) {
    DBG() << "DUP result: " << result << "\n";
  }

  return -errno;
}

long elkvm_do_nanosleep(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->nanosleep == NULL) {
    ERROR() << "NANOSLEEP handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  CURRENT_ABI::paramtype req_p;
  CURRENT_ABI::paramtype rem_p;
  vmi->unpack_syscall(&req_p, &rem_p);

  struct timespec *req = NULL;
  struct timespec *rem = NULL;

  if(req_p != 0x0) {
    req = reinterpret_cast<struct timespec *>(vmi->get_region_manager()->get_pager().get_host_p(req_p));
  }
  if(rem_p != 0x0) {
    rem = reinterpret_cast<struct timespec *>(vmi->get_region_manager()->get_pager().get_host_p(rem_p));
  }

  long result = vmi->get_handlers()->nanosleep(req, rem);
  if(vmi->debug_mode()) {
    DBG() << "NANOSLEEP";
    Elkvm::dbg_log_result<int>(result);
  }

  return result;
}

long elkvm_do_getpid(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->getpid == NULL) {
    return -ENOSYS;
  }

  long pid = vmi->get_handlers()->getpid();
  if(vmi->debug_mode()) {
    DBG() << "GETPID";
    DBG() << "RESULT: " << pid << "\n";
  }

  return pid;
}

long elkvm_do_uname(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->uname == NULL) {
    return -ENOSYS;
  }
  struct utsname *buf = NULL;
  CURRENT_ABI::paramtype bufp = 0;
  vmi->unpack_syscall(&bufp);

  assert(bufp != 0x0);
  buf = (struct utsname *)vmi->get_region_manager()->get_pager().get_host_p(bufp);
  assert(buf != NULL && "host buffer address cannot be NULL in uname");

  long result = vmi->get_handlers()->uname(buf);
  if(vmi->debug_mode()) {
    DBG() << "UNAME buf at: " << (void*)bufp << " (" << (void*)buf << ")";
    DBG() << "sysname " << buf->sysname << " nodename " << buf->nodename
          << " release " << buf->release << " version " << buf->version
          << " machine " << buf->machine;
    Elkvm::dbg_log_result<int>(result);
  }
  return result;
}

long elkvm_do_fcntl(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->fcntl == NULL) {
    ERROR() << "FCNTL handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  CURRENT_ABI::paramtype fd = 0;
  CURRENT_ABI::paramtype cmd = 0;
  /*
   * depending on the value of cmd arg is either an int or a pointer
   * to a struct flock or a pointer to a struct f_owner_ex
   */
  CURRENT_ABI::paramtype arg_p = 0;

  vmi->unpack_syscall(&fd, &cmd, &arg_p);

  long result = 0;
  switch(cmd) {
    case F_GETOWN_EX:
    case F_SETOWN_EX:
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW: {
      /* NULL statement */;
      void *arg = vmi->get_region_manager()->get_pager().get_host_p(arg_p);
      result = vmi->get_handlers()->fcntl(fd, cmd, arg);
      break;
                   }
    default:
      result = vmi->get_handlers()->fcntl(fd, cmd, arg_p);
      break;
  }

  if(vmi->debug_mode()) {
    DBG() << "FCNTL with fd: " << fd << " cmd: " << cmd << " arg_p: " << (void*)arg_p;
    Elkvm::dbg_log_result<int>(result);
    if(result < 0) {
      ERROR() << "ERROR No: " << errno << " Msg: " << strerror(errno);
    }
  }

  return result;
}

long elkvm_do_truncate(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->truncate == NULL) {
    ERROR() << "TRUNCATE handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  CURRENT_ABI::paramtype path_p = 0;
  CURRENT_ABI::paramtype length;
  char *path = NULL;

  vmi->unpack_syscall(&path_p, &length);

  path = reinterpret_cast<char *>(vmi->get_region_manager()->get_pager().get_host_p(path_p));
  long result = vmi->get_handlers()->truncate(path, length);
  if(vmi->debug_mode()) {
    DBG() << "TRUNCATE with path at: " << (void*)path << " (" << path << ") "
          << " length " << length;
    Elkvm::dbg_log_result<int>(result);
    if(result < 0) {
      ERROR() << "ERROR No: " << errno << " Msg: " << strerror(errno);
    }
  }
  return result;
}

long elkvm_do_ftruncate(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->ftruncate == NULL) {
    ERROR() << "FTRUNCATE handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  CURRENT_ABI::paramtype fd = 0;
  CURRENT_ABI::paramtype length;

  vmi->unpack_syscall(&fd, &length);

  long result = vmi->get_handlers()->ftruncate(fd, length);
  if(vmi->debug_mode()) {
    DBG() << "FTRUNCATE with fd: " << fd << " len " << length;
    Elkvm::dbg_log_result<int>(result);
    if(result < 0) {
      ERROR() << "ERROR No: " << errno << " Msg: " << strerror(errno);
    }
  }
  return result;
}

long elkvm_do_getdents(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->getdents == NULL) {
    INFO() <<"GETDENTS handler not found\n";
    return -ENOSYS;
  }

  CURRENT_ABI::paramtype fd = 0;
  guestptr_t dirp_p = 0x0;
  CURRENT_ABI::paramtype count = 0;

  vmi->unpack_syscall(&fd, &dirp_p, &count);

  struct linux_dirent *dirp = NULL;
  if(dirp_p != 0x0) {
    dirp = reinterpret_cast<struct linux_dirent *>(
        vmi->get_region_manager()->get_pager().get_host_p(dirp_p));
  }

  int res = vmi->get_handlers()->getdents(fd, dirp, count);
  if(vmi->debug_mode()) {
    DBG() << "GETDENTS with fd: " << fd << " dirp " << (void*)dirp_p
          << " (" << (void*)dirp << ") count " << count;
    DBG() << "RESULT: " << res << "\n";
    if(res < 0) {
      ERROR() << "ERROR No: " << errno << " Msg: " << strerror(errno);
    }
  }
  if(res < 0) {
    return -errno;
  }
  return res;
}

long elkvm_do_getcwd(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->getcwd == NULL) {
    ERROR() << "GETCWD handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  CURRENT_ABI::paramtype buf_p = 0;
  CURRENT_ABI::paramtype size = 0;
  char *buf = NULL;

  vmi->unpack_syscall(&buf_p, &size);

  buf = reinterpret_cast<char *>(vmi->get_region_manager()->get_pager().get_host_p(buf_p));

  char *result = vmi->get_handlers()->getcwd(buf, size);
  if(vmi->debug_mode()) {
    DBG() << "GETCWD with buf at: " << (void*)buf_p << " (" << (void*)buf << ") size " << size;
    Elkvm::dbg_log_result<char *>(result);
    if(result == NULL) {
      ERROR() << "ERROR No: " << errno << " Msg: " << strerror(errno);
    }
  }
  if(result == NULL) {
    return errno;
  } else {
    return 0;
  }
}

long elkvm_do_chdir(Elkvm::VM * vmi) {
  CURRENT_ABI::paramtype path;
  vmi->unpack_syscall(&path);
  char *local_path = 0;
  if (path) {
    local_path = reinterpret_cast<char*>(vmi->get_region_manager()->get_pager().get_host_p(path));
  }
  return vmi->get_handlers()->chdir(local_path);
}

long elkvm_do_fchdir(Elkvm::VM * vmi) {
  CURRENT_ABI::paramtype path;
  vmi->unpack_syscall(&path);
  return vmi->get_handlers()->fchdir(path);
}

long elkvm_do_mkdir(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->mkdir == NULL) {
    ERROR() << "MKDIR handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  CURRENT_ABI::paramtype pathname_p = 0;
  CURRENT_ABI::paramtype mode = 0;
  char *pathname = NULL;

  vmi->unpack_syscall(&pathname_p, &mode);

  assert(pathname_p != 0x0);
  pathname = reinterpret_cast<char *>(vmi->get_region_manager()->get_pager().get_host_p(pathname_p));
  long result = vmi->get_handlers()->mkdir(pathname, mode);
  if(vmi->debug_mode()) {
    DBG() << "MKDIR with pathname at: " << (void*)pathname
          << " (" << pathname << ") mode " << mode;
    Elkvm::dbg_log_result<int>(result);
    if(result < 0) {
      ERROR() << "ERROR No: " << errno << " Msg: " << strerror(errno);
    }
  }
  return result;

}

long elkvm_do_unlink(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->unlink == NULL) {
    ERROR() << "UNLINK handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  CURRENT_ABI::paramtype pathname_p = 0;
  char *pathname = NULL;

  vmi->unpack_syscall(&pathname_p);

  assert(pathname_p != 0x0);
  pathname = reinterpret_cast<char *>(vmi->get_region_manager()->get_pager().get_host_p(pathname_p));
  long result = vmi->get_handlers()->unlink(pathname);
  if(vmi->debug_mode()) {
    DBG() << "UNLINK with pathname at: " << (void*)pathname << " (" << pathname << ")";
    Elkvm::dbg_log_result<int>(result);
    if(result < 0) {
      ERROR() << "ERROR No: " << errno << " Msg: " << strerror(errno);
    }
  }
  return result;
}

long elkvm_do_readlink(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->readlink == NULL) {
    ERROR() << "READLINK handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  CURRENT_ABI::paramtype path_p = 0;
  CURRENT_ABI::paramtype buf_p = 0;
  CURRENT_ABI::paramtype bufsiz = 0;
  char *path = NULL;
  char *buf = NULL;

  vmi->unpack_syscall(&path_p, &buf_p, &bufsiz);

  path = reinterpret_cast<char *>(vmi->get_region_manager()->get_pager().get_host_p(path_p));
  buf  = reinterpret_cast<char *>(vmi->get_region_manager()->get_pager().get_host_p(buf_p));
  long result = vmi->get_handlers()->readlink(path, buf, bufsiz);
  if(vmi->debug_mode()) {
    DBG() << "READLINK with path at: " << (void*)path << " (" << path << ") buf at "
          << (void*)buf << " bufsize " << bufsiz;
    Elkvm::dbg_log_result<int>(result);
    if(result < 0) {
      ERROR() << "ERROR No: " << errno << " Msg: " << strerror(errno);
    }
  }
  return result;
}

long elkvm_do_gettimeofday(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->gettimeofday == NULL) {
    UNIMPLEMENTED_SYSCALL;
  }

  CURRENT_ABI::paramtype tv_p = 0;
  CURRENT_ABI::paramtype tz_p = 0;
  vmi->unpack_syscall(&tv_p, &tz_p);

  struct timeval *tv = NULL;
  struct timezone *tz = NULL;

  if(tv_p != 0x0) {
    tv = reinterpret_cast<struct timeval *>(vmi->get_region_manager()->get_pager().get_host_p(tv_p));
  }
  if(tz_p != 0x0) {
    tz = reinterpret_cast<struct timezone *>(vmi->get_region_manager()->get_pager().get_host_p(tz_p));
  }

  long result = vmi->get_handlers()->gettimeofday(tv, tz);
  if(vmi->debug_mode()) {
    DBG() << "GETTIMEOFDAY with timeval: " << LOG_GUEST_HOST(tv_p, tv) << LOG_GUEST_HOST(tz_p, tz);
    Elkvm::dbg_log_result<int>(result);
    if(result == 0) {
      if(tv != NULL) {
        DBG()<< "timeval: tv_sec: " << tv->tv_sec << " tv_usec: " << tv->tv_usec;
      }
      if(tz != NULL) {
        DBG() << "timezone: tz_minuteswest: " << tz->tz_minuteswest << " tz_dsttime " << tz->tz_dsttime;
      }
    } else {
      ERROR() << "ERROR No: " << errno << " Msg: " << strerror(errno);
    }
  }

  return result;
}

long elkvm_do_getrusage(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->getrusage == NULL) {
    ERROR() << "GETRUSAGE handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  CURRENT_ABI::paramtype who = 0;
  CURRENT_ABI::paramtype usage_p = 0x0;
  struct rusage *usage = NULL;

  vmi->unpack_syscall(&who, &usage_p);

  assert(usage_p != 0x0);
  assert(who == RUSAGE_SELF);

  usage = reinterpret_cast<struct rusage *>(vmi->get_region_manager()->get_pager().get_host_p(usage_p));

  long result = vmi->get_handlers()->getrusage(who, usage);
  if(vmi->debug_mode()) {
    DBG() << "RUSAGE with who: " << who << " usage: " << LOG_GUEST_HOST(usage, usage_p);
  }
  return result;
}

long elkvm_do_times(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->times == NULL) {
    ERROR() << "TIMES handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  CURRENT_ABI::paramtype buf_p = 0x0;
  struct tms *buf = NULL;

  vmi->unpack_syscall(&buf_p);
  assert(buf_p != 0x0);

  buf = reinterpret_cast<struct tms *>(vmi->get_region_manager()->get_pager().get_host_p(buf_p));
  assert(buf != NULL);

  long result = vmi->get_handlers()->times(buf);
  if(vmi->debug_mode()) {
    DBG() << "TIMES with buf " << LOG_GUEST_HOST(buf_p, buf);
    DBG() << "Result: " << result;
    if(result >= 0) {
      DBG() << "utime: " << buf->tms_utime
            << " stime: " << buf->tms_stime
            << " cutime: " << buf->tms_cutime
            << " cstime: " << buf->tms_cstime;
    }
  }

  if(result == -1) {
    return -errno;
  } else {
    return result;
  }
}

long elkvm_do_getuid(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->getuid == NULL) {
    ERROR() << "GETUID handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  long result = vmi->get_handlers()->getuid();
  if(vmi->debug_mode()) {
    Elkvm::dbg_log_result<int>(result);
  }

  return result;
}

long elkvm_do_getgid(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->getgid == NULL) {
    ERROR() << "GETGID handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  long result = vmi->get_handlers()->getgid();
  if(vmi->debug_mode()) {
    Elkvm::dbg_log_result<int>(result);
  }

  return result;
}

long elkvm_do_geteuid(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->geteuid == NULL) {
    ERROR() << "GETEUID handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  long result = vmi->get_handlers()->geteuid();
  if(vmi->debug_mode()) {
    Elkvm::dbg_log_result<int>(result);
  }

  return result;
}

long elkvm_do_getegid(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->getegid == NULL) {
    ERROR() << "GETEGID handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  long result = vmi->get_handlers()->getegid();
  if(vmi->debug_mode()) {
    Elkvm::dbg_log_result<int>(result);
  }

  return result;
}

long elkvm_do_arch_prctl(Elkvm::VM * vmi) {
  CURRENT_ABI::paramtype code = 0;
  CURRENT_ABI::paramtype user_addr = 0;
  const auto& vcpu = vmi->get_vcpu(0);

  int err = vcpu->get_sregs();
  if(err) {
    return err;
  }

  vmi->unpack_syscall(&code, &user_addr);
  assert(user_addr != 0x0);

  CURRENT_ABI::paramtype *host_addr = reinterpret_cast<CURRENT_ABI::paramtype *>(
      vmi->get_region_manager()->get_pager().get_host_p(user_addr));
  assert(host_addr != nullptr && "could not get host address in arch_prctl");

  switch(code) {
    case ARCH_SET_FS: {
      Elkvm::Segment fs = vcpu->get_reg(Elkvm::Seg_t::fs);
      fs.set_base(user_addr);
      vcpu->set_reg(Elkvm::Seg_t::fs, fs);
      break;
                      }
    case ARCH_GET_FS:
      *host_addr = vcpu->get_reg(Elkvm::Seg_t::fs).get_base();
      break;
    case ARCH_SET_GS: {
      Elkvm::Segment gs = vcpu->get_reg(Elkvm::Seg_t::gs);
      gs.set_base(user_addr);
      vcpu->set_reg(Elkvm::Seg_t::gs, gs);
      break;
                      }
    case ARCH_GET_GS:
      *host_addr = vcpu->get_reg(Elkvm::Seg_t::gs).get_base();
      break;
    default:
      return -EINVAL;
  }

  err = vcpu->set_sregs();
  if(vmi->debug_mode()) {
    std::string cdes;
    switch(code) {
      case ARCH_SET_FS:
        cdes = "ARCH_SET_FS";
        break;
      case ARCH_GET_FS:
        cdes = "ARCH_GET_FS";
        break;
      case ARCH_SET_GS:
        cdes = "ARCH_SET_GS";
        break;
      case ARCH_GET_GS:
        cdes = "ARCH_GET_GS";
        break;
    }

    DBG() << "ARCH PRCTL with code " << cdes << " (" << code << ")"
      << " addr " << LOG_GUEST_HOST(user_addr, host_addr);
    DBG() << "RESULT " << err;
  }
  return err;
}

long elkvm_do_gettid(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->gettid == NULL) {
    ERROR() << "GETTID handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  long result = vmi->get_handlers()->gettid();
  if(vmi->debug_mode()) {
    DBG() << "GETTID";
    Elkvm::dbg_log_result<int>(result);
  }
  return result;
}

long elkvm_do_time(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->time == NULL) {
    ERROR() << "TIME handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  CURRENT_ABI::paramtype time_p = 0;
  vmi->unpack_syscall(&time_p);

  time_t *time = NULL;
  if(time_p != 0x0) {
    time = reinterpret_cast<time_t *>(vmi->get_region_manager()->get_pager().get_host_p(time_p));
  }

  long result = vmi->get_handlers()->time(time);
  if(vmi->debug_mode()) {
    DBG() << "TIME with arg " << LOG_GUEST_HOST(time_p, time);
    Elkvm::dbg_log_result<int>(result);
  }

  return result;
}

long elkvm_do_futex(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->futex == NULL) {
    ERROR() << "FUTEX handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  CURRENT_ABI::paramtype uaddr_p   = 0x0;
  CURRENT_ABI::paramtype op        = 0;
  CURRENT_ABI::paramtype val       = 0;
  CURRENT_ABI::paramtype timeout_p = 0x0;
  CURRENT_ABI::paramtype uaddr2_p  = 0x0;
  CURRENT_ABI::paramtype val3      = 0;
  int *uaddr = NULL;
  const struct timespec *timeout = NULL;
  int *uaddr2 = NULL;

  vmi->unpack_syscall(&uaddr_p, &op, &val, &timeout_p, &uaddr2_p, &val3);

  if(uaddr_p != 0x0) {
    uaddr = reinterpret_cast<int *>(vmi->get_region_manager()->get_pager().get_host_p(uaddr_p));
  }
  if(timeout_p != 0x0) {
    timeout = reinterpret_cast<const struct timespec *>(vmi->get_region_manager()->get_pager().get_host_p(timeout_p));
  }
  if(uaddr2_p != 0x0) {
    uaddr2 = reinterpret_cast<int *>(vmi->get_region_manager()->get_pager().get_host_p(uaddr2_p));
  }

  INFO() << "FUTEX with uaddr " << LOG_GUEST_HOST(uaddr, uaddr_p)
        << " op " << op << " val " << val << " timeout " << LOG_GUEST_HOST(timeout, timeout_p)
        << " uaddr2 " << LOG_GUEST_HOST(uaddr2, uaddr2_p) << " uaddr3 " << (void*)val3;
  long result = vmi->get_handlers()->futex(uaddr, op, val, timeout, uaddr2, val3);
  if(vmi->debug_mode()) {
    DBG() << "FUTEX with uaddr " << LOG_GUEST_HOST(uaddr, uaddr_p)
          << " op " << op << " val " << val << " timeout " << LOG_GUEST_HOST(timeout, timeout_p)
          << " uaddr2 " << LOG_GUEST_HOST(uaddr2, uaddr2_p) << " uaddr3 " << (void*)val3;
    Elkvm::dbg_log_result<int>(result);
  }

  if(result) {
    return -errno;
  }
  return result;

}

long elkvm_do_epoll_create(Elkvm::VM * vmi) {
  CURRENT_ABI::paramtype size;
  vmi->unpack_syscall(&size);
  return vmi->get_handlers()->epoll_create(size);
}

long elkvm_do_exit_group(Elkvm::VM * vmi) {
  CURRENT_ABI::paramtype status = 0;
  vmi->unpack_syscall(&status);

  vmi->get_handlers()->exit_group(status);
  /* should not be reached... */
  return -ENOSYS;
}

long elkvm_do_epoll_wait(Elkvm::VM * vmi) {
  CURRENT_ABI::paramtype epfd;
  CURRENT_ABI::paramtype events;
  CURRENT_ABI::paramtype maxev;
  CURRENT_ABI::paramtype timeout;
  vmi->unpack_syscall(&epfd, &events, &maxev, &timeout);

  struct epoll_event* local_events = 0;
  if (events != 0) {
    local_events =  reinterpret_cast<struct epoll_event*>(vmi->get_region_manager()->get_pager().get_host_p(events));
  }
  return vmi->get_handlers()->epoll_wait(epfd, local_events, maxev, timeout);
}

long elkvm_do_epoll_ctl(Elkvm::VM * vmi) {
  CURRENT_ABI::paramtype epfd;
  CURRENT_ABI::paramtype op;
  CURRENT_ABI::paramtype fd;
  CURRENT_ABI::paramtype event;
  struct epoll_event *local_event = 0;

  vmi->unpack_syscall(&epfd, &op, &fd, &event);
  if (event != 0) {
    local_event = reinterpret_cast<struct epoll_event*>(vmi->get_region_manager()->get_pager().get_host_p(event));
  }
  return vmi->get_handlers()->epoll_ctl(epfd, op, fd, local_event);
}

long elkvm_do_tgkill(Elkvm::VM * vmi) {
  if(vmi->get_handlers()->tgkill == NULL) {
    ERROR() << "TGKILL handler not found" << LOG_RESET << "\n";
    return -ENOSYS;
  }

  CURRENT_ABI::paramtype tgid = 0x0;
  CURRENT_ABI::paramtype tid = 0x0;
  CURRENT_ABI::paramtype sig = 0x0;

  vmi->unpack_syscall(&tgid, &tid, &sig);

  long result = vmi->get_handlers()->tgkill(tgid, tid, sig);
  if(vmi->debug_mode()) {
    DBG() << "TGKILL with tgid " << tgid << " tid " << tid << " sig " << sig;
    Elkvm::dbg_log_result<int>(result);
  }
  return result;

}
