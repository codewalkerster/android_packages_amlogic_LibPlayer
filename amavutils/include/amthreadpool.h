#ifndef AM_LIBPLAYER_THREAD_POOL
#define  AM_LIBPLAYER_THREAD_POOL
#include <pthread.h>

///#define AMTHREADPOOL_DEBUG
int amthreadpool_thread_usleep_in(int us);

int amthreadpool_thread_wake(pthread_t pid);
int amthreadpool_pool_thread_cancel(pthread_t pid);
int amthreadpool_pool_thread_uncancel(pthread_t pid);
int amthreadpool_thread_cancel(pthread_t pid);
int amthreadpool_thread_uncancel(pthread_t pid);

int amthreadpool_pthread_create(pthread_t * newthread,
                                __const pthread_attr_t * attr,
                                void * (*start_routine)(void *),
                                void * arg);
int amthreadpool_pthread_join(pthread_t thid, void ** ret_val);
int amthreadpool_system_init(void);
int amthreadpool_system_dump_info(void);
int amthreadpool_on_requare_exit(pthread_t pid);



#ifdef AMTHREADPOOL_DEBUG
#define amthreadpool_thread_usleep(us)\
	amthreadpool_thread_usleep_debug(us,__FUNCTION__,__LINE__)
#else
#define amthreadpool_thread_usleep(us)\
	amthreadpool_thread_usleep_in(us)	
#endif
#endif
