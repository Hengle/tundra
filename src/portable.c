#include "portable.h"
#include "engine.h"
#include "util.h"

#if defined(__APPLE__) || defined(linux)
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <stdlib.h>
#include <errno.h>
#include <process.h>
#include <stdio.h>
#include <assert.h>

int pthread_mutex_init(pthread_mutex_t *mutex, void *args)
{
	mutex->handle = (PCRITICAL_SECTION)malloc(sizeof(CRITICAL_SECTION));
	if (!mutex->handle)
		return ENOMEM;
	InitializeCriticalSection((PCRITICAL_SECTION)mutex->handle);
	return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
	DeleteCriticalSection((PCRITICAL_SECTION)mutex->handle);
	free(mutex->handle);
	mutex->handle = 0;
	return 0;
}

int pthread_mutex_lock(pthread_mutex_t *lock)
{
	EnterCriticalSection((PCRITICAL_SECTION)lock->handle);
	return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *lock)
{
	LeaveCriticalSection((PCRITICAL_SECTION)lock->handle);
	return 0;
}

int pthread_cond_init(pthread_cond_t *cond, void *args)
{
	assert(args == NULL);

	if (NULL == (cond->handle = malloc(sizeof(CONDITION_VARIABLE))))
		return ENOMEM;

	InitializeConditionVariable((PCONDITION_VARIABLE) cond->handle);
	return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond)
{
	free (cond->handle);
	return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t* lock)
{
	BOOL result = SleepConditionVariableCS((PCONDITION_VARIABLE) cond->handle, (PCRITICAL_SECTION) lock->handle, INFINITE);
	return !result;
}

int pthread_cond_broadcast(pthread_cond_t *cond)
{
	WakeAllConditionVariable((PCONDITION_VARIABLE) cond->handle);
	return 0;
}

int pthread_cond_signal(pthread_cond_t *cond)
{
	WakeConditionVariable((PCONDITION_VARIABLE) cond->handle);
	return 0;
}

typedef struct
{
	uintptr_t thread;
	pthread_thread_routine func;
	void *input_arg;
	void *result;
	volatile LONG taken;
} thread_data;

enum { W32_MAX_THREADS = 32 };
static thread_data setup[W32_MAX_THREADS];

static thread_data* alloc_thread(void)
{
	int i;
	for (i = 0; i < W32_MAX_THREADS; ++i)
	{
		thread_data* p = &setup[i];
		if (0 == InterlockedExchange(&p->taken, 1))
			return p;
	}

	fprintf(stderr, "fatal: too many threads allocated (limit: %d)\n", W32_MAX_THREADS);
	exit(1);
}

static unsigned thread_start(void *args_)
{
	thread_data* args = (thread_data*) args_;
	args->result = (*args->func)(args->input_arg);
	return 0;
}

int pthread_create(pthread_t *result, void* options, pthread_thread_routine start, void *routine_arg)
{
	thread_data* data = alloc_thread();
	data->func = start;
	data->input_arg = routine_arg;
	data->thread = _beginthreadex(NULL, 0, thread_start, data, 0, NULL);
	result->index = (int) (data - &setup[0]);
	return 0;
}

int pthread_join(pthread_t thread, void **result_out)
{
	thread_data *data = &setup[thread.index];
	assert(thread.index >= 0 && thread.index < W32_MAX_THREADS);
	assert(data->taken);

	if (WAIT_OBJECT_0 == WaitForSingleObject((HANDLE) data->thread, INFINITE))
	{
		CloseHandle((HANDLE) data->thread);
		*result_out = data->result;
		memset(data, 0, sizeof(thread_data));
		return 0;
	}
	else
	{
		return EINVAL;
	}
}

#endif

int
td_mkdir(const char *path)
{
#if defined(__APPLE__) || defined(linux)
	return mkdir(path, 0777);
#elif defined(_WIN32)
	if (!CreateDirectoryA(path, NULL))
	{
		switch (GetLastError())
		{
		case ERROR_ALREADY_EXISTS:
			return 0;
		default:
			return 1;
		}
	}
	else
		return 0;
#else
#error meh
#endif

}

int
fs_stat_file(const char *path, td_stat *out)
{
#if defined(__APPLE__) || defined(linux)
	int err;
	struct stat s;
	if (0 != (err = stat(path, &s)))
		return err;

	out->flags = TD_STAT_EXISTS | ((s.st_mode & S_IFDIR) ? TD_STAT_DIR : 0);
	out->size = s.st_size;
	out->timestamp = s.st_mtime;
	return 0;
#elif defined(_WIN32)
#define EPOCH_DIFF 0x019DB1DED53E8000LL /* 116444736000000000 nsecs */
#define RATE_DIFF 10000000 /* 100 nsecs */
	WIN32_FIND_DATAA data;
	HANDLE h = FindFirstFileA(path, &data);
	unsigned __int64 ft;
	if (h == INVALID_HANDLE_VALUE)
		return 1;
	out->flags = TD_STAT_EXISTS | ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? TD_STAT_DIR : 0);
	out->size = ((unsigned __int64)(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
	ft = ((unsigned __int64)(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
	out->timestamp = (unsigned long) ((ft - EPOCH_DIFF) / RATE_DIFF);
	FindClose(h);
	return 0;
#else
#error meh
	return 0;
#endif
}

int td_move_file(const char *source, const char *dest)
{
#if defined(__APPLE__) || defined(linux)
	return rename(source, dest);
#elif defined(_WIN32)
	if (MoveFileExA(source, dest, MOVEFILE_REPLACE_EXISTING))
		return 0;
	else
		return GetLastError();
#else
#error meh
#endif

}

#if defined(_WIN32)
static double perf_to_secs;
static LARGE_INTEGER initial_time;
#elif defined(__APPLE__) || defined(linux)
static double start_time;
static double dtime_now(void)
{
	static const double micros = 1.0/1000000.0;
	struct timeval t;
	if (0 != gettimeofday(&t, NULL))
		td_croak("gettimeofday failed");
	return t.tv_sec + t.tv_usec * micros;
}
#endif

void td_init_timer(void)
{
#if defined(__APPLE__) || defined(linux)
	start_time = dtime_now();
#elif defined(_WIN32)
	LARGE_INTEGER perf_freq;
	if (!QueryPerformanceFrequency(&perf_freq))
		td_croak("QueryPerformanceFrequency failed: %d", (int) GetLastError());
	if (!QueryPerformanceCounter(&initial_time))
		td_croak("QueryPerformanceCounter failed: %d", (int) GetLastError());
	perf_to_secs = 1.0 / (double) perf_freq.QuadPart;
#endif
}

double td_timestamp(void)
{
#if defined(__APPLE__) || defined(linux)
	return dtime_now() - start_time;
#elif defined(_WIN32)
	LARGE_INTEGER c;
	if (!QueryPerformanceCounter(&c))
		td_croak("QueryPerformanceCounter failed: %d", (int) GetLastError());
	return (double) c.QuadPart * perf_to_secs;
#else
#error Meh
#endif
}

static td_sighandler_info * volatile siginfo;

#if defined(__APPLE__) || defined(linux)
static void* signal_handler_thread_fn(void *arg)
{
	int sig, rc;
	sigset_t sigs;
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGINT);
	sigaddset(&sigs, SIGTERM);
	sigaddset(&sigs, SIGQUIT);
	if (0 == (rc = sigwait(&sigs, &sig)))
	{
		td_sighandler_info* info = siginfo;
		if (info)
		{
			pthread_mutex_lock(info->mutex);
			info->flag = -1;
			switch (sig)
			{
				case SIGINT: info->reason = "SIGINT"; break;
				case SIGTERM: info->reason = "SIGTERM"; break;
				case SIGQUIT: info->reason = "SIGQUIT"; break;
			}
			pthread_mutex_unlock(info->mutex);
			pthread_cond_broadcast(info->cond);
		}
	}
	else
		td_croak("sigwait failed: %d", rc);
	return NULL;
}
#endif

void td_install_sighandler(td_sighandler_info *info)
{
	siginfo = info;

#if defined(__APPLE__) || defined(linux)
	{
		pthread_t sigthread;
		if (0 != pthread_create(&sigthread, NULL, signal_handler_thread_fn, NULL))
			td_croak("couldn't start signal handler thread");
		pthread_detach(sigthread);
	}
#elif defined(_WIN32)
#else
#error Meh
#endif
}

void td_remove_sighandler(void)
{
	siginfo = NULL;
}

void td_block_signals(int block)
{
#if defined(__APPLE__) || defined(linux)
	sigset_t sigs;
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGINT);
	sigaddset(&sigs, SIGTERM);
	sigaddset(&sigs, SIGQUIT);
	if  (0 != pthread_sigmask(block ? SIG_BLOCK : SIG_UNBLOCK, &sigs, 0))
		td_croak("pthread_sigmask failed");
#endif
}

int td_exec(const char* cmd_line, int *was_signalled_out)
{
#if defined(__APPLE__) || defined(linux)
	pid_t child;
	if (0 == (child = fork()))
	{
		sigset_t sigs;
		sigfillset(&sigs);
		if (0 != sigprocmask(SIG_UNBLOCK, &sigs, 0))
			perror("sigprocmask failed");

		const char *args[] = { "/bin/sh", "-c", cmd_line, NULL };

		if (-1 == execv("/bin/sh", (char **) args))
			exit(1);
		/* we never get here */
		abort();
	}
	else if (-1 == child)
	{
		perror("fork failed");
		return 1;
	}
	else
	{
		pid_t p;
		int return_code;
		p = waitpid(child, &return_code, 0);
		if (p != child)
		{
			perror("waitpid failed");
			return 1;
		}
	
		*was_signalled_out = WIFSIGNALED(return_code);
		return return_code;
	}

#elif defined(_WIN32)
	*was_signalled_out = 0;
	return system(cmd_line);
#else
#error meh
#endif
}

