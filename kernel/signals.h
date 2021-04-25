#include "types.h"

#define SIG_DFL 0 /* default signal handling */
#define SIG_IGN 1 /* ignore signal */
#define SIGKILL 9
#define SIGSTOP 17
#define SIGCONT 19
#define SIGNALS_COUNT 32
typedef void handler(int);

struct sigaction {
  void (*sa_handler) (int);
  uint sigmask;
};