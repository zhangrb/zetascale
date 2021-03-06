//----------------------------------------------------------------------------
// ZetaScale
// Copyright (c) 2016, SanDisk Corp. and/or all its affiliates.
//
// This program is free software; you can redistribute it and/or modify it under
// the terms of the GNU Lesser General Public License version 2.1 as published by the Free
// Software Foundation;
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License v2.1 for more details.
//
// A copy of the GNU Lesser General Public License v2.1 is provided with this package and
// can also be found at: http://opensource.org/licenses/LGPL-2.1
// You should have received a copy of the GNU Lesser General Public License along with
// this program; if not, write to the Free Software Foundation, Inc., 59 Temple
// Place, Suite 330, Boston, MA 02111-1307 USA.
//----------------------------------------------------------------------------

#include <zs.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#include <assert.h>


#define ZS_MAX_KEY_LEN 256
#define NUM_OBJS 10 //max mput in single operation
#define NUM_MPUTS 10 

static int cur_thd_id = 0;
static __thread int my_thdid = 0;
static __thread int objs_processed = 0;

ZS_cguid_t cguid;
struct ZS_state *zs_state;
int num_mputs =  NUM_MPUTS;
int num_objs = NUM_OBJS;
int use_mput = 1;
uint32_t flags_global = 0;
int num_thds = 1;
int total_objs = NUM_OBJS;

int cnt_id = 0;
inline uint64_t
get_time_usecs(void)
{
        struct timeval tv = { 0, 0};
        gettimeofday(&tv, NULL);
        return ((tv.tv_sec * 1000 * 1000) + tv.tv_usec);
}

#define LENGTH_INCR 3
#define MAX_KEYLEN 256

int length_incr = LENGTH_INCR;
int change_data = false;

bool range_update_cb(char *key, uint32_t keylen, char *data, uint32_t datalen,
		     void *callback_args, char **new_data, uint32_t *new_datalen)
{
	
	*new_datalen = 0;
	(*new_data) = NULL;

//	printf("Thread id %d got update on key = %s.\n", my_thdid, key);

	objs_processed++; 

	if (!change_data) {
		return false;
	}

	if (length_incr) {
		assert((datalen + length_incr) > 0);
		(*new_data) = (char *) malloc(datalen + length_incr);
		if (*new_data == NULL) {
			return false;
		}	
		memcpy(*new_data, data, datalen);
		(*new_data)[0] = 'J';	//Change in data
		*new_datalen = datalen + length_incr; //Change in data size
	} else {
		data[0] = 'O';
	}

	return true;
}

void
do_range_update(struct ZS_thread_state *thd_state, ZS_cguid_t cguid,
		uint32_t flags, int key_seed)
{
	int i, k;
	ZS_status_t status;
	ZS_obj_t *objs = NULL; 
	uint64_t num_zs_reads = 0;
	uint64_t num_zs_mputs = 0;
	uint32_t objs_written = 0;
	char *data;
	uint64_t data_len;
	uint64_t key_num = 0;
	uint64_t mismatch = 0;
	char range_key[MAX_KEYLEN] = {0};
	uint32_t range_key_len = 0;
	ZS_range_update_cb_t callback_func = range_update_cb;
	void *callback_args = NULL;
	uint32_t objs_updated = 0;

	objs = (ZS_obj_t *) malloc(sizeof(ZS_obj_t) * num_objs);
	if (objs == NULL) {
		printf("Cannot allocate memory.\n");
		exit(0);
	}
	memset(objs, 0, sizeof(ZS_obj_t) * num_objs);
	for (i = 0; i < num_objs; i++) {
		objs[i].key = malloc(ZS_MAX_KEY_LEN);
		if (objs[i].key == NULL) {
			printf("Cannot allocate memory.\n");
			exit(0);
		}
		objs[i].data = malloc(1024);
		if (objs[i].data == NULL) {
			printf("Cannot allocate memory.\n");
			exit(0);
		}
	}

	num_mputs = total_objs / num_objs;
	printf("Doing Mput in threads %d.\n", my_thdid);
	for (k = 1; k <= num_mputs; k++) {

		for (i = 0; i < num_objs; i++) {
			memset(objs[i].key, 0, ZS_MAX_KEY_LEN);
			sprintf(objs[i].key, "key_%02d_%06"PRId64"", my_thdid, key_num);
			sprintf(objs[i].data, "key_%02d_%06"PRId64"", my_thdid, key_num);

			key_num += key_seed;
			objs[i].key_len = strlen(objs[i].key) + 1;
			objs[i].data_len = strlen(objs[i].data) + 1;
			objs[i].flags = 0;
		}

		status = ZSMPut(thd_state, cguid, num_objs,
				 &objs[0], flags, &objs_written);
		if (status != ZS_SUCCESS) {
			printf("Failed to write objects using ZSMPut, status = %d.\n",
				status);
			assert(0);
			return ;
		}
		num_zs_mputs++;
	}

	printf("Written %"PRId64" objs in thread %d.\n", num_zs_mputs * num_objs, my_thdid);

	num_zs_reads = 0;
	
	printf("Reading all objects put in thread = %d.\n", my_thdid);
	key_num = 0;
	for (k = 1; k <= num_mputs; k++) {

		for (i = 0; i < num_objs; i++) {
			memset(objs[i].key, 0, ZS_MAX_KEY_LEN);

			sprintf(objs[i].key, "key_%02d_%06"PRId64"", my_thdid, key_num);
			sprintf(objs[i].data, "key_%02d_%06"PRId64"", my_thdid, key_num);

			key_num += key_seed;

			objs[i].key_len = strlen(objs[i].key) + 1;
			objs[i].data_len = strlen(objs[i].data) + 1;
			objs[i].flags = 0;

			status = ZSReadObject(thd_state, cguid,
					       objs[i].key, objs[i].key_len,
						&data, &data_len);
			if (status != ZS_SUCCESS) {
					printf("Read failed with %d errror.\n", status);
					assert(0);
					exit(0);
			}

			if (data_len != objs[i].data_len) {
				assert(data_len <= 13);
				printf("Object length of read object mismatch.\n");	
				assert(0);
				mismatch++;
			}
			num_zs_reads++;
		}
	}

	printf("Verified the %"PRId64" mput objects using reads, mismatch = %"PRId64".\n", num_zs_reads, mismatch);

	printf("Doing range update on all objects........\n");

	objs_processed = 0;

	sprintf(range_key, "key_%02d_", my_thdid);
	range_key_len = strlen(range_key);

	status = ZSRangeUpdate(thd_state, cguid, range_key, range_key_len,
				callback_func, callback_args, NULL,
				NULL, &objs_updated);
	if (status != ZS_SUCCESS) {
		printf("Failed to do range update.\n");
		assert(0);
		exit(-1);
	}

	assert(objs_processed == num_zs_reads);

	printf("Done range update on %d objects.\n", objs_updated);

	printf("Reading all objects updated in thread = %d.\n", my_thdid);
	key_num = 0;
	for (k = 1; k <= num_mputs; k++) {

		for (i = 0; i < num_objs; i++) {
			memset(objs[i].key, 0, ZS_MAX_KEY_LEN);

			sprintf(objs[i].key, "key_%02d_%06"PRId64"", my_thdid, key_num);
			sprintf(objs[i].data, "key_%02d_%06"PRId64"", my_thdid, key_num); //Changed k to j in range update

			key_num += key_seed;

			objs[i].key_len = strlen(objs[i].key) + 1;
			objs[i].data_len = strlen(objs[i].data) + 1;
			objs[i].flags = 0;

			status = ZSReadObject(thd_state, cguid,
					       objs[i].key, objs[i].key_len,
					       &data, &data_len);
			if (status != ZS_SUCCESS) {
					printf("Read failed with %d errror.\n", status);
					assert(0);
					exit(0);
			}

			if (data_len != (objs[i].data_len + length_incr)) {
				printf("Object length of read object mismatch.\n");	
				assert(0);
				mismatch++;
			}
			num_zs_reads++;
		}
	}

	printf("Verified all objects updated in thread = %d.\n", my_thdid);
	
}

void *
range_update_stress(void *t)
{
	struct ZS_thread_state *thd_state;

	my_thdid = __sync_fetch_and_add(&cur_thd_id, 1);
	ZSInitPerThreadState(zs_state, &thd_state);	

	do_range_update(thd_state, cguid, ZS_WRITE_MUST_NOT_EXIST, 1);

	ZSReleasePerThreadState(&thd_state);

	return NULL;
}


typedef void * (* thd_func_t) (void *);

void launch_thds()
{
	int i;
	pthread_t thread_id[128];

	sleep(1);

	for(i = 0; i < num_thds; i++) {
		fprintf(stderr,"Creating thread %i\n",i );
		if( pthread_create(&thread_id[i], NULL, range_update_stress, NULL)!= 0 ) {
		    perror("pthread_create: ");
		    exit(1);
		}
	}

	for(i = 0; i < num_thds; i++) {
		if( pthread_join(thread_id[i], NULL) != 0 ) {
			perror("pthread_join: ");
			exit(1);
		} 
	}
}

void
do_op(uint32_t flags_in, int length) 
{
	ZS_container_props_t props;
	struct ZS_thread_state *thd_state;
	ZS_status_t status;
	char cnt_name[100] = {0};
	sprintf(cnt_name, "cntr_%d", cnt_id++);


	ZSInitPerThreadState(zs_state, &thd_state);	
	ZSLoadCntrPropDefaults(&props);

	props.persistent = 1;
	props.evicting = 0;
	props.writethru = 1;
	props.durability_level= 0;
	props.fifo_mode = 0;
	props.size_kb = (1024 * 1024 * 10);;

	status = ZSOpenContainer(thd_state, cnt_name, &props, ZS_CTNR_CREATE, &cguid);
	if (status != ZS_SUCCESS) {
		printf("Open Cont failed with error=%x.\n", status);
		exit(-1);
	}

	flags_global = flags_in;

	printf("############### Doing op with length change = %d, modify = %d  #########.\n", length, change_data);
	length_incr = length;
	launch_thds(); //actual operations that keeps object size sameA

	ZSCloseContainer(thd_state, cguid);
	ZSDeleteContainer(thd_state, cguid);

	ZSReleasePerThreadState(&thd_state);
}

int 
main(int argc, char *argv[])
{
	int n, m;

	if (argc < 3) {
		printf("Usage: ./run total_objs(multiple of 1000) num_thds.\n");
		exit(0);
	}

	use_mput = atoi(argv[1]);
	m = atoi(argv[1]);
	if (m > NUM_OBJS) {
		total_objs = m;	
	}
	n = atoi(argv[2]);
	if (n > 0 && n < 40) {
		num_thds = n;
	}

	printf("Running with total_objs = %d, num threads = %d.\n",
		total_objs, num_thds);

	ZSInit(&zs_state);

	do_op(0, 0);// set

	change_data = true;

	do_op(0, 0);// set
	do_op(0, 3);// set
	do_op(0, 30);// set
	do_op(0, -3);// set
	do_op(0, -10);// set

	ZSShutdown(zs_state);
	return 0;
}

