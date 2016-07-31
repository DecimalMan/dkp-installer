#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "common.h"
#include <zlib.h>

int cmdfd;
char *zip_path;
#ifndef RECOVERY_BUILD
char *bootimg_path;
#endif

static pthread_t threads[THREAD_COUNT];
static pthread_mutex_t thread_lock = PTHREAD_MUTEX_INITIALIZER;

static void *(*const thread_funcs[])(void *) = {
	[THREAD_ZIMAGE] = unpack_zimage,
	[THREAD_SYSTEM] = unpack_system,
	[THREAD_GENSPLASH] = generate_splash,
	[THREAD_RAMDISK] = generate_ramdisk,
};

void mod_prio(pthread_t thread, int new_pol) {
	struct sched_param param;
	param.sched_priority = 0;
	if (pthread_setschedparam(thread, new_pol, &param))
		rprint("Couldn't set scheduler policy!");
}

int main(int argc, char **argv) {
	int ret = 0;
	struct stat check_zip;
	int i;
	pthread_attr_t th_attr;

#ifdef RECOVERY_BUILD
	if (argc != 4 || stat(argv[3], &check_zip) == -1)
		return 1;

	cmdfd = atoi(argv[2]);
	zip_path = argv[3];
#else
	if (argc != 3 ||
		stat(argv[1], &check_zip) == -1 ||
		stat(argv[2], &check_zip) == -1)
		return 1;

	cmdfd = 0;
	zip_path = argv[1];
	bootimg_path = argv[2];
#endif

	get_crc_table();
	signal(SIGPIPE, SIG_IGN);

	pthread_attr_init(&th_attr);
	pthread_attr_setdetachstate(&th_attr, PTHREAD_CREATE_JOINABLE);

	pthread_mutex_lock(&thread_lock);

	for (i = THREAD_COUNT - 1; i >= 0; i--) {
		ret = pthread_create(&threads[i], &th_attr, thread_funcs[i], NULL);
		if (ret)
			goto abort_cancel_and_die;
	}

	pthread_mutex_unlock(&thread_lock);

	mod_prio(threads[THREAD_ZIMAGE], SCHED_BATCH);
	mod_prio(threads[THREAD_SYSTEM], SCHED_BATCH);

	pthread_attr_destroy(&th_attr);
	ret = generate_bootimg();
	if (ret)
		goto just_die;

	rprint("Completing installation");
	ret = wait_thread(THREAD_SYSTEM);

	return -ret;

abort_cancel_and_die:
	rprint("Aborting threads");
	for (i++; i < THREAD_COUNT; i++) {
		pthread_cancel(threads[i]);
	}
just_die:
	rprint("Installation failed");
	return -ret;
}

int wait_thread(enum thread_slot slot) {
	long retval;
	int join_ret;
	pthread_t this_thread;

	pthread_mutex_lock(&thread_lock);
	this_thread = threads[slot];
	pthread_mutex_unlock(&thread_lock);
	join_ret = pthread_join(this_thread, (void **)&retval);
	if (join_ret)
		return join_ret;
	else
		return (int)retval;
}
