// Guest-only answers to C library calls whose system calls the sandbox does
// not provide - by design: it has no directories, no host clock and no host
// scheduler, and a call it does not know ends the guest. One static link, so
// defining them here takes the place of musl's.
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>

extern "C" {

// A guest's files are the ones it was given, by name. Anything the emulator
// would make, remove or rename on a disk is told the disk is read-only.
int mkdir(const char*, mode_t) {
  errno = EROFS;
  return -1;
}
int rmdir(const char*) {
  errno = EROFS;
  return -1;
}
int unlink(const char*) {
  errno = EROFS;
  return -1;
}
int rename(const char*, const char*) {
  errno = EROFS;
  return -1;
}
int chmod(const char*, mode_t) {
  errno = EROFS;
  return -1;
}
int fchmod(int, mode_t) { return 0; }

// There is one directory and it is the root.
char* getcwd(char* buf, size_t size) {
  if (!buf || size < 2) {
    errno = ERANGE;
    return nullptr;
  }
  buf[0] = '/';
  buf[1] = '\0';
  return buf;
}

int uname(struct utsname* u) {
  if (!u) {
    errno = EFAULT;
    return -1;
  }
  memset(u, 0, sizeof *u);
  strcpy(u->sysname, "Chimera");
  strcpy(u->nodename, "waterbox");
  strcpy(u->release, "1");
  strcpy(u->version, "1");
  strcpy(u->machine, "x86_64");
  return 0;
}

// The machine has one processor to run on, and saying so keeps anything
// sized by the count the same everywhere. musl defines the four affinity
// functions in one object; replacing one means providing all of them.
int sched_getaffinity(pid_t, size_t cpusetsize, cpu_set_t* mask) {
  if (!mask || cpusetsize < sizeof(unsigned long)) {
    errno = EINVAL;
    return -1;
  }
  memset(mask, 0, cpusetsize);
  CPU_SET(0, mask);
  return 0;
}
int sched_setaffinity(pid_t, size_t, const cpu_set_t*) { return 0; }
int pthread_setaffinity_np(pthread_t, size_t, const cpu_set_t*) { return 0; }
int pthread_getaffinity_np(pthread_t, size_t cpusetsize, cpu_set_t* mask) {
  return sched_getaffinity(0, cpusetsize, mask);
}

// Priorities are a host scheduler's business, and there is none.
int pthread_setschedparam(pthread_t, int, const struct sched_param*) {
  return 0;
}
int pthread_getschedparam(pthread_t, int* policy, struct sched_param* p) {
  if (policy) {
    *policy = 0;
  }
  if (p) {
    p->sched_priority = 0;
  }
  return 0;
}
int sched_get_priority_max(int) { return 0; }
int sched_get_priority_min(int) { return 0; }

// Whoever asks the system for randomness gets a fixed stream.
ssize_t getrandom(void* buf, size_t n, unsigned) {
  static uint64_t x = 0x9E3779B97F4A7C15ull;
  uint8_t* p = static_cast<uint8_t*>(buf);
  for (size_t i = 0; i < n; i++) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    p[i] = static_cast<uint8_t>(x);
  }
  return static_cast<ssize_t>(n);
}

}  // extern "C"
