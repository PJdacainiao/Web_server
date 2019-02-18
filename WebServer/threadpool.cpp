#include "threadpool.h"
#include <pthread.h>
#include <iostream>

static void *threadpool_thread(void* threadpool);
threadpool_t *threadpool_create(int thread_count, int queue_size, int flags)
{
	threadpool_t *pool;
	do{
		if((thread_count<=0 || thread_count > MAX_THREADS) || (queue_size<=0 || queue_size > MAX_QUEUE))
			return NULL;
		if((pool = (threadpool_t *)malloc(sizeof(threadpool_t))) == NULL)
			break;  //分配资源失败后边要释放空间

		pool->thread_count = 0;
		pool->queue_size = queue_size;
		pool->count = 0;
		pool->head = 0;
		pool->tail = 0;
		pool->shutdown = 0;
		pool->started = 0;

		pool->threads = (pthread_t*)malloc(thread_count*sizeof(pthread_t));
		pool->queue = (threadpool_task_t*)malloc(queue_size * sizeof(threadpool_task_t));
		
		if(pool->threads==NULL || pool->queue==NULL)
			break;
        
		if((pthread_mutex_init(&(pool->lock), NULL) !=0) ||(pthread_cond_init(&(pool->notify), NULL) != 0))
		    break;
		for(int i = 0; i < thread_count; ++i){
			if(pthread_create(&(pool->threads[i]), NULL, threadpool_thread, (void*)pool) != 0)
			{
				threadpool_destroy(pool, 0);
				return NULL;
			}
			pool->thread_count += 1;
			pool->started += 1;
		}
		printf("threadpoll initialized\n");
		return pool;	
	}while(false);

	if(pool != NULL)
		threadpool_free(pool);
	return NULL;
}

int threadpool_add(threadpool_t *pool, void (*function)(void*), void *argument, int flags)
{
    int err = 0;
	if(pool == NULL || function == NULL)
		return THREADPOOL_INVALID;
	if(pthread_mutex_lock(&(pool->lock)) != 0)
		return THREADPOOL_LOCK_FAILURE;
	do{
		
	        if(pool->count >= pool->queue_size){
		      err =  THREADPOOL_QUEUE_FULL;
		      break;
		}
                if(pool->shutdown == 1){
			err = THREADPOOL_SHUTDOWN;
			break;
		}

	        pool->queue[pool->tail].function = function;
	        pool->queue[pool->tail].argument = argument;
	        pool->tail = (pool->tail + 1) % pool->queue_size;
	        pool->count += 1;
			//printf("task->count:%d\n", pool->count);
                /*添加任务通知线程*/
        	if(pthread_cond_signal(&(pool->notify)) != 0){
	        	err = THREADPOOL_LOCK_FAILURE;
			break;
		}

	}while(false); //do while中break 和 err 是为了unlock

	if(pthread_mutex_unlock(&(pool->lock)) != 0 )
		err = THREADPOOL_LOCK_FAILURE;

	return err;
}


int threadpool_destroy(threadpool_t *pool, int flags){
	printf("Thread pool destroy!\n");
	int err = 0;
	if(pool == NULL)
		return THREADPOOL_INVALID;
	if(pthread_mutex_lock(&(pool->lock)) != 0)
		return THREADPOOL_LOCK_FAILURE;

	do{
		/* 已经关闭*/
	        if(pool->shutdown){
                       err =  THREADPOOL_SHUTDOWN;
		       break;
		}
                pool->shutdown = (flags & THREADPOOL_GRACEFUL)? graceful_shutdown : immediate_shutdown;
                /* wakeup all the threads*/
		if(pthread_cond_broadcast(&pool->notify) != 0 || pthread_mutex_unlock(&(pool->lock)) != 0){
			err = THREADPOOL_LOCK_FAILURE;
			break;
		}
		/* 等待所有线程结束*/
		for(int i=0; i<pool->started; ++i){
			if(pthread_join(pool->threads[i], NULL) != 0){
				err = THREADPOOL_THREAD_FAILURE;
			}
		}
	}while(false);

	if(!err)
		threadpool_free(pool);
	return err;
}

int threadpool_free(threadpool_t *pool)
{
	if(pool == NULL || pool->started > 0)
		return -1;
        if(pool->threads){
		free(pool->threads);
		free(pool->queue);
		/*lock the mutex in case*/
	        pthread_mutex_lock(&(pool->lock));
        	pthread_mutex_destroy(&(pool->lock));
        	pthread_cond_destroy(&(pool->notify));
	}
	free(pool);
	return 0;
}

static void *threadpool_thread(void *threadpool)
{
	threadpool_t *pool = (threadpool_t*)threadpool;
	threadpool_task_t task;
        /*循环，一个任务结束又等待下一个*/
	for(;;)
	{	
	pthread_mutex_lock(&(pool->lock));
	while(pool->count == 0 && !(pool->shutdown))
		pthread_cond_wait(&(pool->notify), &(pool->lock));
        	
	if(pool->shutdown == immediate_shutdown || (pool->shutdown == graceful_shutdown && pool->count == 0))
		break;

	task.function = pool->queue[pool->head].function;
	task.argument = pool->queue[pool->head].argument;
	pool->count -= 1;
	pool->head = (pool->head + 1) % pool->queue_size;
	pthread_mutex_unlock(&(pool->lock));
	//printf("function run!\n");
	(*(task.function))(task.argument);
	}
        /*线程退出*/
	--pool->started;

	pthread_mutex_unlock(&(pool->lock));
	pthread_exit(NULL);
	return(NULL);
}

            
