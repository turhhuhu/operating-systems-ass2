
struct counting_semaphore{
    int first_sem_desc;
    int second_sem_desc;
    int value;
};


void csem_down(struct counting_semaphore *sem);
void csem_up(struct counting_semaphore *sem);
int csem_alloc(struct counting_semaphore *sem, int initial_value);
void csem_free(struct counting_semaphore *sem);