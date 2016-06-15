/*
 *Library: simple thread pool 
 *Function: head file of this lib
 *Author: Chong Zhao
 *E-mail: zhaochong@pku.edu.cn
*/

#ifndef SIM_THREAD_POOL_H
#define SIM_THREAD_POOL_H

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

typedef int sim_bool;
typedef void * sim_job_param;
typedef void (*sim_job_handler)(void *);
typedef struct _sim_thread_pool sim_thread_pool;

sim_thread_pool *sim_create_thread_pool(int min_thread_num, int max_thread_num);
sim_bool sim_destroy_thread_pool(sim_thread_pool **pp_thread_pool);
sim_bool sim_process_new_job(sim_thread_pool *p_thread_pool,
				sim_job_handler job_handler, sim_job_param p_job_param);

#endif /* end of SIM_THREAD_POOL_H */ 
