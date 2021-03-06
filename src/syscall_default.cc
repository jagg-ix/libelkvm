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

#include <asm/unistd_64.h>
#include <errno.h>
#include <fcntl.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <linux/futex.h>
#include <linux/unistd.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

#include <elkvm/elkvm.h>
#include <elkvm/syscall.h>

long pass_read(int fd, void *buf, size_t count) {
  return read(fd, buf, count);
}

long pass_write(int fd, void *buf, size_t count) {
  return write(fd, buf, count);
}

long pass_open(const char *pathname, int flags, mode_t mode) {
  return open(pathname, flags, mode);
}

long pass_close(int fd) {
  return close(fd);
}

long pass_stat(const char *path, struct stat *buf) {
  return stat(path, buf);
}

long pass_fstat(int fd, struct stat *buf) {
  return fstat(fd, buf);
}

long pass_lstat(const char *path, struct stat *buf) {
  return lstat(path, buf);
}

long pass_lseek(int fd, off_t offset, int whence) {
  return lseek(fd, offset, whence);
}

long allow_sigaction(int signum __attribute__((unused)),
    const struct sigaction *act __attribute__((unused)),
    struct sigaction *oldact __attribute__((unused))) {
  return 1;
}

long pass_sigprocmask(int how, const sigset_t *set, sigset_t *old)
{
	return sigprocmask(how, set, old);
}

long pass_ioctl(int fd, unsigned long request, char *argp) {
  return ioctl(fd, request, argp);
}

long pass_munmap(struct region_mapping *mapping) {
  return munmap(mapping->host_p, mapping->length);
}

long pass_readv(int fd, struct iovec *iov, int iovcnt) {
  return readv(fd, iov, iovcnt);
}

long pass_writev(int fd, struct iovec *iov, int iovcnt) {
  return writev(fd, iov, iovcnt);
}

long pass_access(const char *pathname, int mode) {
  return access(pathname, mode);
}

long pass_pipe(int pipefds[2]) {
  return pipe(pipefds);
}

long pass_dup(int oldfd) {
  return dup(oldfd);
}

long pass_nanosleep(const struct timespec* req, struct timespec* rem)
{
  return nanosleep(req, rem);
}

long pass_getpid() {
  return getpid();
}

long pass_getuid() {
  return getuid();
}

long pass_getgid() {
  return getgid();
}

long pass_geteuid() {
  return geteuid();
}

long pass_getegid() {
  return getegid();
}

long pass_uname(struct utsname *buf) {
  return uname(buf);
}

long pass_fcntl(int fd, int cmd, ...) {
  va_list ap;
  long result = 0;
  void *parg = NULL;
  int iarg = 0;

  va_start(ap, cmd);
  switch(cmd) {
    case F_GETOWN_EX:
    case F_SETOWN_EX:
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW:
      parg = va_arg(ap, void *);
      result = fcntl(fd, cmd, parg);
      break;
    default:
      iarg = va_arg(ap, int);
      result = fcntl(fd, cmd, iarg);
      break;
  }

  va_end(ap);
  return result;
}

long pass_truncate(const char *path, off_t length) {
  return truncate(path, length);
}

long pass_ftruncate(int fd, off_t length) {
  return ftruncate(fd, length);
}

int pass_getdents(unsigned fd, struct linux_dirent *dirp, unsigned count) {
  return syscall(__NR_getdents, fd, dirp, count);
}

char *pass_getcwd(char *buf, size_t size) {
  return getcwd(buf, size);
}

int pass_chdir(char const *path) {
  return chdir(path);
}

int pass_fchdir(int fd) {
  return fchdir(fd);
}

long pass_mkdir(const char *pathname, mode_t mode) {
  return mkdir(pathname, mode);
}

long pass_unlink(const char *pathname) {
  return unlink(pathname);
}

long pass_readlink(const char *path, char *buf, size_t bufsiz) {
  return readlink(path, buf, bufsiz);
}

long pass_gettimeofday(struct timeval *tv, struct timezone *tz) {
  return gettimeofday(tv, tz);
}

long pass_getrusage(int who, struct rusage *usage) {
  return getrusage(who, usage);
}

long pass_statfs(const char *path, struct statfs *buf) {
  return statfs(path, buf);
}

int pass_fstatfs(int fd, struct statfs *buf) {
  return fstatfs(fd, buf);
}

long pass_setrlimit(int resource, const struct rlimit *rlim) {
  return setrlimit(resource, rlim);
}

long pass_gettid() {
  return syscall(__NR_gettid);
}

long pass_time(time_t *t) {
  return time(t);
}

long pass_futex(int *uaddr, int op, int val, const struct timespec *timeout,
    int *uaddr2, int val3) {
  return syscall(__NR_futex, uaddr, op, val, timeout, uaddr2, val3);
}

long pass_clock_gettime(clockid_t clk_id, struct timespec *tp) {
  return clock_gettime(clk_id, tp);
}

int pass_clock_getres(clockid_t clk_id, struct timespec *res) {
  return clock_getres(clk_id, res);
}

void pass_exit_group(int status) {
  exit(status);
}

long pass_tgkill(int tgid, int tid, int sig) {
  return syscall(__NR_tgkill, tgid, tid, sig);
}

int pass_openat(int dirfd, const char *pathname, int flags) {
  return openat(dirfd, pathname, flags);
}

long pass_socket(int domain, int type, int protocol) {
  return socket(domain, type, protocol);
}

long pass_bind(int sock, struct sockaddr const *addr, socklen_t addrlen) {
  return bind(sock, addr, addrlen);
}

long pass_accept(int sock, struct sockaddr* addr, socklen_t *len) {
  return accept(sock, addr, len);
}

long pass_listen(int sock, int backlog) {
  return listen(sock, backlog);
}

long pass_getsockname(int sock, struct sockaddr* addr, socklen_t *addrlen) {
  return getsockname(sock, addr, addrlen);
}

long pass_setsockopt(int sock, int lvl, int optname, const void* optval,
					 socklen_t optlen) {
  return setsockopt(sock, lvl, optname, optval, optlen);
}

long pass_epoll_create(int size) {
  return epoll_create(size);
}

long pass_set_tid_address(int *tidptr) {
  return syscall(__NR_set_tid_address, tidptr);
}

long pass_epoll_ctl(int epfd, int op, int fd, struct epoll_event* event) {
  return epoll_ctl(epfd, op, fd, event);
}


long pass_epoll_wait(int epfd, struct epoll_event* events, int max, int timeout) {
  return epoll_wait(epfd, events, max, timeout);
}

long pass_set_robust_list(struct robust_list_head *head, size_t len) {
  return syscall(__NR_set_robust_list, head, len);
}

Elkvm::elkvm_handlers
Elkvm::default_handlers = {
  .read = pass_read,
  .write = pass_write,
  .open = pass_open,
  .close = pass_close,
  .stat = pass_stat,
  .fstat = pass_fstat,
  .lstat = pass_lstat,
  .poll = NULL,
  .lseek = pass_lseek,
  .mmap_before = NULL,
  .mmap_after = NULL,
  .mprotect = NULL,
  .munmap = pass_munmap,
  /* ... */
  .sigaction = allow_sigaction,
  .sigprocmask = pass_sigprocmask,
  .ioctl = pass_ioctl,
  /* ... */
  .readv = pass_readv,
  .writev = pass_writev,
  .access = pass_access,
  .pipe = pass_pipe,
  .dup = pass_dup,
  /* ... */
  .nanosleep = pass_nanosleep,
  /* ... */
  .getpid = pass_getpid,
  /* ... */
  .socket = pass_socket,
  .accept = pass_accept,
  .bind = pass_bind,
  .listen = pass_listen,
  .getsockname = pass_getsockname,
  /* ... */
  .setsockopt = pass_setsockopt,
  /* ... */
  .getuid  = pass_getuid,
  .getgid  = pass_getgid,
  .geteuid = pass_geteuid,
  .getegid = pass_getegid,
  /* ... */
  .uname = pass_uname,
  .fcntl = pass_fcntl,
  .truncate = pass_truncate,
  .ftruncate = pass_ftruncate,
  .getdents = pass_getdents,
  .getcwd = pass_getcwd,
  .chdir = pass_chdir,
  .fchdir = pass_fchdir,
  .mkdir = pass_mkdir,
  .unlink = pass_unlink,
  .readlink = pass_readlink,
  /* ... */
  .gettimeofday = pass_gettimeofday,
  .getrusage = pass_getrusage,
  .times = NULL,
  /* ... */
  .statfs = pass_statfs,
  .fstatfs = pass_fstatfs,
  /* ... */
  .setrlimit = pass_setrlimit,
  /* ... */
  .gettid = pass_gettid,
  .time = pass_time,
  .futex = pass_futex,
  /* ... */
  .epoll_create = pass_epoll_create,
  .set_tid_address = pass_set_tid_address,
  .epoll_ctl = pass_epoll_ctl,
  .epoll_wait = pass_epoll_wait,
  /* ... */
  .clock_gettime = pass_clock_gettime,
  .clock_getres  = pass_clock_getres,
  .exit_group = pass_exit_group,
  .tgkill = pass_tgkill,
  .openat = pass_openat,
  /* ... */
  .set_robust_list = pass_set_robust_list,

  .bp_callback = NULL,
};

Elkvm::hypercall_handlers
Elkvm::hypercall_null = {
	.pre_handler = NULL,
	.post_handler = NULL,
};

