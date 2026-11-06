// RFC 0038 §4: a program that saves the runtime's SIGSEGV handler, installs
// its own, and in it restores the saved one and raises the signal again
// (Python's faulthandler does) dies of the signal, as it would without
// WeaveC, rather than raising it forever. The child's alarm ends a handler
// that would spin.
// RUN: %weavec_cc -O2 %s -o %t
// RUN: %t | FileCheck %s
// CHECK: child: signal 11
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static struct sigaction saved;

static void restoreAndRaise(int signal) {
  (void)sigaction(SIGSEGV, &saved, NULL);
  (void)raise(signal);
}

int main(void) {
  int status = 0;
  const pid_t child = fork();
  if (child == 0) {
    struct sigaction mine;
    (void)alarm(10);
    memset(&mine, 0, sizeof mine);
    mine.sa_handler = restoreAndRaise;
    sigemptyset(&mine.sa_mask);
    (void)sigaction(SIGSEGV, &mine, &saved);
    (void)raise(SIGSEGV);
    _exit(0);
  }
  if (child < 0 || waitpid(child, &status, 0) != child)
    return 2;
  if (WIFSIGNALED(status))
    printf("child: signal %d\n", WTERMSIG(status));
  else
    printf("child: exit %d\n", WEXITSTATUS(status));
  return 0;
}
