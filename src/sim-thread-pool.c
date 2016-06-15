#include <stdio.h>  
#include <stdlib.h>  
#include <sys/types.h>  
#include <pthread.h>  
#include <signal.h>  
#include <errno.h>
#include <assert.h>
#include <memory.h>

#include "sim-thread-pool.h"
#include "list.h"

#define SIM_MAX_THREAD_NUM 1000
#define MANAGE_INTERVAL 1
#define MANAGE_RATIO 0.8

#define DBG_MESSAGE(args...) \
	do{ \
	char b__[1024]; \
	sprintf(b__, args); \
	fprintf(stdout,"Message: [function:%s, line:%d]%s", \
			__FUNCTION__, __LINE__, b__); \
	}while(0)

#define DBG_WARNING(args...) \
	do{ \
	char b__[1024]; \
	sprintf(b__, args); \
	fprintf(stdout,"Warning: [function:%s, line:%d]%s", \
			__FUNCTION__, __LINE__, b__); \
	}while(0)

#define DBG_ERROR(args...) \
	do{ \
	char b__[1024]; \
	sprintf(b__, args); \
	fprintf(stdout,"Error: [function:%s, line:%d]%s", \
			__FUNCTION__, __LINE__, b__); \
	}while(0)

typedef struct _sim_thread_info sim_thread_info;

struct _sim_thread_info {
	pthread_t thread_id;
	sim_bool busy;
	sim_bool exit;
	sim_job_handler job_handler;
	sim_job_param job_param;

	pthread_cond_t thread_cond;
	pthread_mutex_t thread_mutex;
};

struct _sim_thread_pool {
	int min_thread_num;
	int cur_thread_num;
	int max_thread_num;

	sim_thread_info *p_thread_info;

	pthread_mutex_t pool_mutex;
	pthread_t manage_id;

	list_head task_list;
};

struct _sim_task {
	sim_job_handler job_handler;
	sim_job_param job_param;
	list_head sim_task_node;
};
typedef struct _sim_task sim_task;

static sim_bool sim_init_thread_pool(sim_thread_pool *p_thread_pool);
static sim_thread_info *sim_create_thread(sim_thread_pool *p_thread_pool);
static void sim_destroy_thread(sim_thread_info *p_thread_info);
static void *sim_work_thread(void *param);
static void *sim_manage_thread(void *param);
static void sim_thread_quit_handler(int signal);
static sim_bool  sim_process_new_job_with_thread(sim_thread_info *p_thread_info,
				sim_job_handler job_handler, void *job_param);

sim_thread_pool *sim_create_thread_pool(int min_thread_num, int max_thread_num)
{	
	sim_thread_pool *p_thread_pool;
	if (min_thread_num < 0 || min_thread_num >= max_thread_num
			|| max_thread_num > SIM_MAX_THREAD_NUM) {
		return NULL;
	}

	p_thread_pool = (sim_thread_pool *)malloc(sizeof(sim_thread_pool));
	if (p_thread_pool == NULL) {
		DBG_ERROR("Allocate sim_thread_pool failed.\n");
		return NULL;
	}

	p_thread_pool->min_thread_num = min_thread_num;
	p_thread_pool->max_thread_num = max_thread_num;
	p_thread_pool->cur_thread_num = 0;
	
	if (sim_init_thread_pool(p_thread_pool)) {
		return p_thread_pool;
	}
	else {
		DBG_ERROR("Init thread pool failed.\n");
		free(p_thread_pool);
		return NULL;
	}		
}

sim_bool sim_destroy_thread_pool(sim_thread_pool **pp_thread_pool)
{
	sim_thread_pool *p_thread_pool;
	int i;

	if (pp_thread_pool == NULL)
		return FALSE;
	p_thread_pool = *pp_thread_pool;
	if (p_thread_pool == NULL)
		return FALSE;

	pthread_mutex_lock(&p_thread_pool->pool_mutex);
	for (i = 0; i < p_thread_pool->cur_thread_num; i++) {	
		sim_destroy_thread(&p_thread_pool->p_thread_info[i]);
	}
	pthread_mutex_unlock(&p_thread_pool->pool_mutex);

	pthread_kill(p_thread_pool->manage_id, SIGQUIT);
	pthread_mutex_destroy(&p_thread_pool->pool_mutex);

	free(p_thread_pool->p_thread_info);
	free(p_thread_pool);

	return TRUE;
}

sim_bool sim_process_new_job(sim_thread_pool *p_thread_pool,
				sim_job_handler job_handler, void *job_param)
{
	sim_thread_info *p_thread_info;
	sim_task *p_task;
	sim_bool result;
	int i;

	if (p_thread_pool == NULL || job_handler == NULL) {
		DBG_WARNING("p_thread_pool is null or job_handler is null\n");
		return FALSE;
	}

	p_thread_info = p_thread_pool->p_thread_info;
	for (i = 0; i < p_thread_pool->cur_thread_num; i++) {
		if (p_thread_info[i].busy == FALSE && p_thread_info[i].exit == FALSE) {
			pthread_mutex_lock(&p_thread_pool->pool_mutex);
			result = sim_process_new_job_with_thread(
							&p_thread_info[i],
							job_handler,
							job_param);
			pthread_mutex_unlock(&p_thread_pool->pool_mutex);
			if (result == TRUE) {
				return TRUE;
			}
		}
	}	

	pthread_mutex_lock(&p_thread_pool->pool_mutex);
	p_thread_info = sim_create_thread(p_thread_pool);
	if (p_thread_info == NULL) {
		pthread_mutex_unlock(&p_thread_pool->pool_mutex);
		goto fail;
	}
	else {
		p_thread_pool->cur_thread_num++;
		result = sim_process_new_job_with_thread(p_thread_info,
						job_handler, job_param);
		pthread_mutex_unlock(&p_thread_pool->pool_mutex);
		if (result == TRUE) {
			return TRUE;
		}
	}

fail:
	DBG_MESSAGE("Pool is full used, add task node to task list\n");
	p_task = (sim_task *)malloc(sizeof(sim_task));
	p_task->job_handler = job_handler;
	p_task->job_param = job_param;
	assert(p_task->job_handler != NULL && p_task->job_param != NULL);
	pthread_mutex_lock(&p_thread_pool->pool_mutex);
	list_add_tail(&p_task->sim_task_node, &p_thread_pool->task_list);	
	pthread_mutex_unlock(&p_thread_pool->pool_mutex);

	return FALSE;
}

static sim_bool sim_init_thread_pool(sim_thread_pool *p_thread_pool)
{
	sim_thread_info *p_thread_info;
	int min_thread_num;
	void *status;
	int i, j;
	int err;

	assert(p_thread_pool != NULL);
	min_thread_num = p_thread_pool->min_thread_num;
	p_thread_info = (sim_thread_info *)
		malloc(sizeof(sim_thread_info) * p_thread_pool->max_thread_num);
	if (p_thread_info == NULL) {
		DBG_ERROR("Allocate sim_thread_info failed.\n");
		return FALSE;
	}
	else {
		p_thread_pool->p_thread_info = p_thread_info;
	}

	for (i = 0; i < p_thread_pool->max_thread_num; i++) {
		p_thread_info[i].busy = TRUE;
		p_thread_info[i].exit = TRUE;
		memset(&p_thread_info[i].thread_id, '\0', sizeof(pthread_t));
	}

	/* init manager mutex */
	pthread_mutex_init(&p_thread_pool->pool_mutex, NULL);

	/* create work thread */
	pthread_mutex_lock(&p_thread_pool->pool_mutex);
	for (i = 0; i < min_thread_num; i++) {
		p_thread_info = sim_create_thread(p_thread_pool);
		if (p_thread_info == NULL) {
			DBG_ERROR("Create new thread failed.\n");
			pthread_mutex_unlock(&p_thread_pool->pool_mutex);
			goto failed;
		}
		p_thread_pool->cur_thread_num++;
	}
	pthread_mutex_unlock(&p_thread_pool->pool_mutex);

	/* create manage thread */
	err = pthread_create(&p_thread_pool->manage_id, NULL,
			sim_manage_thread, p_thread_pool);
	if (err != 0) {
		DBG_ERROR("Create manage thread failed.\n");
		goto failed; 
	}

	init_list_head(&p_thread_pool->task_list);
	return TRUE;

failed:
	pthread_mutex_lock(&p_thread_pool->pool_mutex);
	for (j = 0; j < i; j++) {
		sim_destroy_thread(&p_thread_pool->p_thread_info[i]);
		p_thread_pool->cur_thread_num--;
	}
	pthread_mutex_unlock(&p_thread_pool->pool_mutex);

	free(p_thread_pool->p_thread_info);
	pthread_mutex_destroy(&p_thread_pool->pool_mutex);

	return FALSE;
}

static void *sim_work_thread(void *param)  
{ 
	sim_thread_info *p_thread_info;

	p_thread_info = (sim_thread_info *)param;
	p_thread_info->busy = TRUE;

	signal(SIGQUIT, sim_thread_quit_handler);
	while (TRUE) { 
		pthread_mutex_lock(&p_thread_info->thread_mutex);
		p_thread_info->busy = FALSE;
		pthread_cond_wait(&p_thread_info->thread_cond,
					&p_thread_info->thread_mutex);
		p_thread_info->busy = TRUE;

		if (p_thread_info->job_handler != NULL) {
			DBG_MESSAGE("Start to exec job.\n");
			p_thread_info->job_handler(p_thread_info->job_param);
		}

		p_thread_info->job_handler = NULL;
		p_thread_info->job_param = NULL;
		pthread_mutex_unlock(&p_thread_info->thread_mutex);

		if (p_thread_info->exit) {
			DBG_MESSAGE("The thread is going to exit.\n");
			return;
		}
	}
}

static void *sim_manage_thread(void *param)
{
	sim_thread_pool *p_thread_pool;
	list_head *task_list;
	list_head *pos, *tmp;
	sim_task *p_task;
	int cur_thread_num;
	int min_thread_num;
	int max_thread_num;
	int idle_thread_num;
	float ratio;
	sim_bool ret;
	int i;


	p_thread_pool = (sim_thread_pool *)param;
	assert(p_thread_pool != NULL);
	DBG_MESSAGE("Start manager thread\n");

	signal(SIGQUIT, sim_thread_quit_handler);
	min_thread_num = p_thread_pool->min_thread_num;
	max_thread_num = p_thread_pool->max_thread_num;
	while (TRUE) {
		sleep(MANAGE_INTERVAL);
		DBG_MESSAGE("Manage thread is awake\n");		

		idle_thread_num = 0;
		task_list = &p_thread_pool->task_list;

		pos = task_list->next;
		while (pos != task_list) {
			tmp = pos;
			pos = pos->next;
			p_task = list_entry(tmp, sim_task, sim_task_node);

			pthread_mutex_lock(&p_thread_pool->pool_mutex);
			list_del(tmp);
			pthread_mutex_unlock(&p_thread_pool->pool_mutex);

			ret = sim_process_new_job(p_thread_pool, p_task->job_handler, p_task->job_param);
			free(p_task);	
			if (ret == FALSE)
				break;
		}

		if (task_list != task_list->next)
			continue;

		for (i = 0; i < max_thread_num; i++) {
			if (p_thread_pool->p_thread_info[i].busy == FALSE
			&& p_thread_pool->p_thread_info[i].exit == FALSE) {
				idle_thread_num++;
			}
		}

		ratio = idle_thread_num / ((float)max_thread_num);
		if (ratio < MANAGE_RATIO) {
			continue;
		}
		else {
			DBG_MESSAGE("Too many idle threads exit, desroy some of them\n");
			cur_thread_num = p_thread_pool->cur_thread_num;
			for (i = 0; i < max_thread_num, cur_thread_num > min_thread_num; i++) {
				if (p_thread_pool->p_thread_info[i].busy == FALSE
					&& p_thread_pool->p_thread_info[i].exit == FALSE) {

					pthread_mutex_lock(&p_thread_pool->pool_mutex);
					if (p_thread_pool->p_thread_info[i].busy == TRUE
						|| p_thread_pool->p_thread_info[i].exit == TRUE) {
						pthread_mutex_unlock(&p_thread_pool->pool_mutex);
						continue;
					}
					sim_destroy_thread(&p_thread_pool->p_thread_info[i]);
					p_thread_pool->cur_thread_num--;
					cur_thread_num = p_thread_pool->cur_thread_num;
					pthread_mutex_unlock(&p_thread_pool->pool_mutex);
				}
			}

		}
	}	
}

static sim_thread_info *sim_create_thread(sim_thread_pool *p_thread_pool)
{
	sim_thread_info *p_thread_info;
	int cur_thread_num;
	int max_thread_num;
	int err;
	int i;

	assert(p_thread_pool != NULL);
	cur_thread_num = p_thread_pool->cur_thread_num;
	max_thread_num = p_thread_pool->max_thread_num;
	if (cur_thread_num >= max_thread_num) {
		DBG_WARNING("Current thread num is up to the limit.\n");
		return NULL;
	}

	for (i = 0; i < max_thread_num; i++) {
		p_thread_info = &p_thread_pool->p_thread_info[i];
		if (p_thread_info->exit == TRUE) {
			break;
		}
	}
	if (i >= max_thread_num)
		return NULL;

	p_thread_info = &p_thread_pool->p_thread_info[i];
	p_thread_info->job_handler = NULL;
	p_thread_info->job_param = NULL;
	p_thread_info->busy = TRUE;
	p_thread_info->exit = FALSE;
	pthread_mutex_init(&p_thread_info->thread_mutex, NULL);
	pthread_cond_init(&p_thread_info->thread_cond, NULL);
	assert(p_thread_info != NULL);
	err = pthread_create(&p_thread_info->thread_id, NULL,
			sim_work_thread, p_thread_info);
	if (err != 0) {
		DBG_ERROR("System call pthread_create failed.\n");
		goto failed;
	}

	/* wait until the thread block at "p_thread_info = false" */
	while (p_thread_info->busy == TRUE) {
	}

	return p_thread_info;

failed:
	pthread_mutex_destroy(&p_thread_info->thread_mutex);
	pthread_cond_destroy(&p_thread_info->thread_cond);

	return NULL;
}

static void sim_destroy_thread(sim_thread_info *p_thread_info)
{
	assert(p_thread_info != NULL);

	if (p_thread_info->exit == TRUE) {
		return;
	}

	/* wait for thread finish working */
	while (p_thread_info->busy != FALSE) {
	}

	pthread_mutex_lock(&p_thread_info->thread_mutex);
	p_thread_info->exit = TRUE;	
	pthread_cond_signal(&p_thread_info->thread_cond);
	pthread_mutex_unlock(&p_thread_info->thread_mutex);

	pthread_mutex_destroy(&p_thread_info->thread_mutex);
	pthread_cond_destroy(&p_thread_info->thread_cond);

	pthread_join(p_thread_info->thread_id, NULL);
}

static void sim_thread_quit_handler(int signal)
{ 
	pthread_t cur_id;
	
	cur_id = pthread_self();
	pthread_exit(NULL);
}

static sim_bool  sim_process_new_job_with_thread(sim_thread_info *p_thread_info,
				sim_job_handler job_handler, void *job_param)
{
	assert(p_thread_info != NULL);
	
	pthread_mutex_lock(&p_thread_info->thread_mutex);
	if (p_thread_info->busy == TRUE || p_thread_info->exit == TRUE) {
		pthread_mutex_unlock(&p_thread_info->thread_mutex);
		return FALSE;
	}

	p_thread_info->job_handler = job_handler;
	p_thread_info->job_param = job_param;
	p_thread_info->busy = TRUE;
	pthread_cond_signal(&p_thread_info->thread_cond);
	pthread_mutex_unlock(&p_thread_info->thread_mutex);

	return TRUE;
}

