#include "kernel/types.h"
#include "user.h"
#include "Csemaphore.h"

int csem_alloc(struct counting_semaphore *sem, int initial_value){
    if((initial_value < 0) || ((sem->first_sem_desc = bsem_alloc()) < 0) || ((sem->second_sem_desc = bsem_alloc()) < 0)){
        return -1;
    }
    sem->value = initial_value;
    if(initial_value == 0){
        bsem_down(sem->second_sem_desc);
    }
    return 0;
}

void csem_free(struct counting_semaphore *sem){
    bsem_free(sem->first_sem_desc);
    sem -> first_sem_desc = -1;
    bsem_free(sem->second_sem_desc);
    sem -> second_sem_desc = -1;
    sem->value = 0;
}

void csem_up(struct counting_semaphore *sem){
    bsem_down(sem->first_sem_desc);
    sem->value = sem->value + 1;
    if(sem->value == 1){
        bsem_up(sem->second_sem_desc);
    }
    bsem_up(sem->first_sem_desc);

}

void csem_down(struct counting_semaphore *sem){
    bsem_down(sem->second_sem_desc);
    bsem_down(sem->first_sem_desc);
    sem->value = sem->value - 1;
    if(sem->value > 0){
        bsem_up(sem->second_sem_desc);
    }
    bsem_up(sem->first_sem_desc);

}
