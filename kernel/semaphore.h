#include "spinlock.h"
#include "sleeplock.h"

#define MAX_BSEM 128

enum semaphorestate { UNUSEDS, USEDS };

struct semaphore {
  struct sleeplock sl;
  int descriptor;
  enum semaphorestate state;
};

struct semaphore_table{
	struct spinlock lock;
	struct semaphore sems[MAX_BSEM];
};