#define _GNU_SOURCE

#include <errno.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pthread.h>
#include <numa.h>

#include <sched.h>
#include <time.h>

#include <math.h> 

#define DEFAULT_MEMSIZE 1048576 //1MB
#define DEFAULT_LOOPS 1000
#define DEFAULT_NODE 0 //numa node
#define DEFAULT_THREADS 1

#define handle_error_en(en, msg) \
do { errno = en; perror(msg); exit(EXIT_FAILURE); } while (0)

/********* strcuts *********/

struct memory_t
{
    void *buf;
    size_t size;
    int numa_node;
};

struct config_t
{
    size_t mem_size;
    int numa_node;
    int numa_dist;
    int num_threads;
    long long num_loops;
};

struct task_t
{
    int idx_task;
    long long num_loops;
    void *mem;
    size_t size;
    void (*function_ptr)(struct memory_t*, int,int);
};

/********** global variables **********/

struct config_t config = {
    DEFAULT_MEMSIZE,
    DEFAULT_NODE,
    DEFAULT_THREADS,
    DEFAULT_LOOPS
};

pthread_mutex_t start_mutex;
pthread_cond_t start_cond;
int cnt_threads;
cpu_set_t cpuset;
struct runtime_t *rtimes;

/********** methods **********/

// allocate memory
void
alloc_memory(size_t size, struct memory_t *mem, int numa_node, int numa_dist)
{
    //allocate memory pages on local numa node
    if(numa_node>=0 && numa_dist==0)
    {
        printf("Allocating %zu Bytes locally on NUMA node %d\n", size, numa_node);
        mem->buf = numa_alloc_onnode(size, numa_node);
    }
    //allocate memory pages on other numa node
    else if(numa_node>=0 && numa_dist>0)
    {
        int mem_numa_node = (numa_node+numa_dist)%(numa_max_node()+1);
        printf("Allocating %zu Bytes remotely on NUMA node %d\n", size, mem_numa_node);
        mem->buf = numa_alloc_onnode(size, mem_numa_node);
    }
    //allocate memory pages wo any policy
    else
    {
	printf("Allocating %zu Bytes w/o NUMA policy\n", size);
        mem->buf = malloc(size);
    }
    
    if (mem->buf == NULL)
    {
        handle_error_en(errno, "memalloc error");
    }
    mem->size = size;
    mem->numa_node = numa_node;
    
    //create memory pages on numa node
    memset (mem->buf, 0, size);
}

// free memory
void
free_memory(struct memory_t *mem)
{
    if(mem->numa_node>=0)
    {
        numa_free(mem->buf, mem->size);
    }
    else
    {
        free(mem->buf);
    }
}

// read values in memory using 64 bit semantics (simple)
int
read_memory64(void* memarea, size_t size, size_t repeats)
{
    uint64_t* begin = (uint64_t*)memarea;
    uint64_t* end = begin + size / sizeof(uint64_t);
    uint64_t value;
    
    do {
        uint64_t* p = begin;
        do {
            value = *p++;
        }
        while (p < end);
    }
    while (--repeats != 0);
    
    return value;
}

// read values in memory using 64 bit semantics (unrolled)
int
read_memory64_uroll(void* memarea, size_t size, size_t repeats)
{
    uint64_t* begin = (uint64_t*)memarea;
    uint64_t* end = begin + size / sizeof(uint64_t) - 3*sizeof(uint64_t);
    uint64_t value;

    do {
        uint64_t* p = begin;
        do {
            value = *p;
            value = *(p+1);
	    value = *(p+2);
            value = *(p+3);
            p = p+4;
        }
        while (p < end);
    }
    while (--repeats != 0);

    return value;
}

// 64-bit reader in an indexed loop (assembler)
void read_memory64_asm(void* memarea, size_t size, size_t repeats)
{
    asm("1: \n" // start of repeat loop
        "xor    %%rcx, %%rcx \n"        // rcx = reset index
        "2: \n" // start of read loop
        "mov    (%[memarea],%%rcx), %%rax \n"
        "add    $8, %%rcx \n"
        // test read loop condition
        "cmp    %[size], %%rcx \n"      // compare to total size
        "jb     2b \n"
        // test repeat loop condition
        "dec    %[repeats] \n"          // until repeats = 0
        "jnz    1b \n"
        :
        : [memarea] "r" (memarea), [size] "r" (size), [repeats] "r" (repeats)
        : "rax", "rcx");
}

// transform bitmask to cpuset
void
bitmask2cpuset(struct bitmask *bm, cpu_set_t *cpuset)
{
    CPU_ZERO(cpuset);
    
    int i;
    for(i=0; i<bm->size; ++i){
        if(numa_bitmask_isbitset(bm, i))
            CPU_SET(i, cpuset);
    }
}

// get cpuset for all CPUs
void
allcpuset(int num_cpu, cpu_set_t *cpuset)
{
    CPU_ZERO(cpuset);
    
    int i;
    for(i=0; i<num_cpu; ++i){
        CPU_SET(i, cpuset);
    }
}

// print bitmask
void print_cpuset(int num_cpu, cpu_set_t *cpuset)
{
    int i=0;
    for(i=0; i<num_cpu; ++i)
        printf("%d", CPU_ISSET(i, cpuset));
}

// execute task
void*
do_work(void *param)
{
    pthread_mutex_lock(&start_mutex);
    struct task_t *task = (struct task_t*)param;
    
    // set cpu mask according to NUMA region
    pthread_t thread = pthread_self();
    int rc = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
    if (rc != 0)
        handle_error_en(rc, "pthread_setaffinity_np error");
    
    // wait for all other threads
    cnt_threads++;
    pthread_cond_wait(&start_cond, &start_mutex);
    pthread_mutex_unlock(&start_mutex);
    
    // run task
    printf("Executing task %d from %p to %p \n", task->idx_task, task->mem, task->mem+task->size);
    int i;
    (*task->function_ptr)(task->mem, task->size, task->num_loops);
    
    pthread_exit(NULL);
}

// create threads to execute a given function f
void
run_threads(int num_threads, int num_loops, void* f, struct memory_t *mem, int elem_size)
{
    //init cpu mask
    int num_cpus = numa_num_configured_cpus();
    if(mem->numa_node>=0){
        struct bitmask* bm = numa_bitmask_alloc(num_cpus);
        numa_node_to_cpus(mem->numa_node, bm);
        bitmask2cpuset(bm, &cpuset);
    }
    else
    {
        allcpuset(num_cpus, &cpuset);
    }
    printf("Set CPU affinity to ");print_cpuset(num_cpus, &cpuset);printf("\n");
    
    //create threads
    pthread_t threads[num_threads];
    struct task_t tasks[num_threads];
    
    pthread_mutex_init(&start_mutex, NULL);
    pthread_cond_init(&start_cond, NULL);
    
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    
    int i;
    int chunk_size = mem->size / num_threads;
    
    for(i=0;i<num_threads;++i)
    {
        tasks[i].idx_task = i;
        tasks[i].num_loops = num_loops;
        tasks[i].size = chunk_size;
        tasks[i].mem = mem->buf+(i*chunk_size);
        tasks[i].function_ptr = f;
        
        int rc = pthread_create(&threads[i], &attr, do_work, &tasks[i]);
        if(rc){
            handle_error_en(rc, "pthread_create error");
        }
    }
    
    // wait for threads to start
    int wait = 1;
    do
    {
        pthread_mutex_lock(&start_mutex);
        wait = (cnt_threads<num_threads);
        pthread_mutex_unlock(&start_mutex);
    } while(wait);
    
    // signal threads to run
    struct timeval tv_start;
    struct timeval tv_end;
    gettimeofday(&tv_start, NULL);
    pthread_cond_broadcast(&start_cond);
    
    // wait for threads to finish
    for (i=0; i<num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    gettimeofday(&tv_end, NULL);
    
    // clean up
    pthread_attr_destroy(&attr);
    pthread_mutex_destroy(&start_mutex);
    pthread_cond_destroy(&start_cond);
    
    // calulcate runtime
    long long start_us = (tv_start.tv_sec % 86400) * 1000000 + tv_start.tv_usec;
    long long end_us = (tv_end.tv_sec % 86400) * 1000000 + tv_end.tv_usec;
    long double lat_us = ((long double)(end_us - start_us)) / num_loops;
    long double bw_mbs = ((long double)mem->size/(1024*1024))/(lat_us/1000000);
    
    //write results
    printf("%Lf\t%zu\t%Lf\n", lat_us, mem->size, bw_mbs);
    
    FILE *out_file;
    out_file = fopen("stats.csv", "a");
    fprintf(out_file, "%zu\t%d\t%Lf\t%Lf\n", mem->size, num_threads, lat_us, bw_mbs);
    fclose(out_file);
}

/********** main **********/

int
main (int argc, char *argv[])
{
    // print usage
    if ( argc != 7 )
    {
        /* We print argv[0] assuming it is the program name */
        printf( "usage: %s \n\tnum_loops \n\tnum_threads \n\tmem_size(in 2^x Bytes) \
		\n\tnuma_node(0...n or -1 for do not care) \n\tnuma_dist(0=local, 1...n=remote)\
		\n\tfunction(=read_memory64,read_memory64_asm,read_memory64_uroll) \n", argv[0] );
        return 0;
    }
    
    // get arguments
    config.num_loops = atoll(argv[1]);
    config.num_threads = atoi(argv[2]);
    config.mem_size = (long long)pow(2, atof(argv[3]));
    config.numa_node = atoi(argv[4]);
    config.numa_dist = atoi(argv[5]);
    
    void *f;
    if(strcmp(argv[6],"read_memory64")==0)
    {
        f = &read_memory64;
    }
    else if(strcmp(argv[6],"read_memory64_uroll")==0)
    {
        f = &read_memory64_uroll;
    }
    else if(strcmp(argv[6],"read_memory64_asm")==0)
    {
        f = &read_memory64_asm;
    }
    else
    {
        handle_error_en(22, "Function name unknown");
    }
    
    // run benchmark
   printf("Running: %s %llu %d %zu %d %d %s \n", \
        argv[0], config.num_loops, config.num_threads, config.mem_size, \
        config.numa_node, config.numa_dist, argv[6]);
 
    struct memory_t read;
    alloc_memory(config.mem_size, &read, config.numa_node, config.numa_dist);
    
    run_threads(config.num_threads, config.num_loops, f , &read, sizeof(uint64_t));
    
    free_memory(&read);
}
