#include <asm/unistd_64.h>
#include <errno.h>
#include <fcntl.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <time.h>

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

void pass_exit_group(int status) {
  exit(status);
}

long pass_tgkill(int tgid, int tid, int sig) {
  return syscall(__NR_tgkill, tgid, tid, sig);
}

int pass_openat(int dirfd, const char *pathname, int flags) {
  return openat(dirfd, pathname, flags);
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
  .sigprocmask = NULL,
  /* ... */
  .readv = pass_readv,
  .writev = pass_writev,
  .access = pass_access,
  .pipe = pass_pipe,
  .dup = pass_dup,
  /* ... */
  .nanosleep = NULL,
  /* ... */
  .getpid = pass_getpid,
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
  .mkdir = pass_mkdir,
  .unlink = pass_unlink,
  .readlink = pass_readlink,
  /* ... */
  .gettimeofday = pass_gettimeofday,
  .getrusage = pass_getrusage,
  .times = NULL,
  /* ... */
  .statfs = pass_statfs,
  /* ... */
  .gettid = pass_gettid,
  .time = pass_time,
  .futex = pass_futex,
  /* ... */
  .clock_gettime = pass_clock_gettime,
  .exit_group = pass_exit_group,
  .tgkill = pass_tgkill,
  .openat = pass_openat,

  .bp_callback = NULL,
};

Elkvm::hypercall_handlers
Elkvm::hypercall_null = {
	.pre_handler = NULL,
	.post_handler = NULL,
};

