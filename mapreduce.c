/*
 * Implementation file for simple MapReduce framework.  Fill in the functions
 * and definitions in this file to complete the assignment.
 *
 * Place all of your implementation code in this file.  You are encouraged to
 * create helper functions wherever it is necessary or will make your code
 * clearer.
 * For these functions, you should follow the practice of declaring
 * them "static" and not including them in the header file (which should only be
 * used for the *public-facing* API.
 */

/* Header includes */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <fcntl.h>
#include "mapreduce.h"

/* Size of shared memory buffers */
#define MR_BUFFER_SIZE 1024

struct args_helper{									// The args for map function
 struct map_reduce *mr;
 int infd, outfd, nmaps, id;
 map_fn map;
 reduce_fn reduce;
};

struct buffer_node{
  void *kv;
  uint32_t keysz;
	uint32_t valuesz;
  struct buffer_node *next;
};

/*	Helper function that can be passed to the pthread_create to call the map_fn
 */
static void *map_wrapper(void* map_args) {
  struct args_helper *args = (struct args_helper *) map_args;
  args->mr->mapfn_failed[args->id] = args->map(args->mr, args->infd, args->id, args->nmaps);
  pthread_exit((void*) &args->mr->mapfn_failed[args->id]);
  //return (void *)map_args;
}

/*	Helper function that can be passed to the pthread_create to call the reduce_fn
 */
static void *reduce_wrapper(void* reduce_args) {
  struct args_helper *args = (struct args_helper *) reduce_args;
  args->mr->reducefn_failed = args->reduce(args->mr, args->outfd, args->nmaps);
  pthread_exit((void*) &args->mr->reducefn_failed);
  //return (void *)reduce_args;
}

struct map_reduce*
mr_create(map_fn map, reduce_fn reduce, int threads) {
   // http://stackoverflow.com/questions/29350073/invalid-write-of-size-8-after-a-malloc

   struct map_reduce *mr = malloc (sizeof(struct map_reduce));
   if(mr == NULL) return NULL;

   mr->map = map;// Save the function inside the sturcture
   mr->reduce = reduce;
   mr->n_threads = threads;// Save the static data

   // give meaningless init value
   mr->outfd = -1;
   mr->reducefn_failed = -1;
   mr->reduce_thread_failed = -1;
   mr->outfd_failed = -1;

   mr->args = malloc (sizeof(struct args_helper) * (threads + 1));
   if(mr->args == NULL) return NULL;

   mr->infd = malloc (sizeof(int) * threads);
   if(mr->infd == NULL) return NULL;

   mr->map_threads = malloc(sizeof(pthread_t) * threads);
   if(mr->map_threads == NULL) return NULL;

   mr->mapfn_failed= malloc(sizeof(int) * threads);
   if(mr->mapfn_failed == NULL) return NULL;
   else {
     for(int i=0; i<threads; i++)
       mr->mapfn_failed[i] = -1;
   }

   mr->map_thread_failed = malloc(sizeof(int) * threads);
   if(mr->map_thread_failed == NULL) return NULL;
   else {
     for(int i=0; i<threads; i++)
       mr->map_thread_failed[i] = -1;
   }

   mr->map_return_values = malloc(threads * sizeof(void*));
      if (mr->map_return_values == NULL) return NULL;
      else {
        for (int i=0; i<threads; i++)
          mr->map_return_values[i] = (void*)(intptr_t)-1;
      }

   mr->infd_failed = malloc(sizeof(int) * threads);
   if(mr->infd_failed == NULL)return NULL;
   else {
     for(int i=0; i<threads; i++)
       mr->infd_failed[i] = -1;
   }
   mr->_lock = malloc(threads * sizeof(pthread_mutex_t));
   if (mr->_lock == NULL) return NULL;

   mr->map_cv = malloc(threads * sizeof(pthread_cond_t));
   if(mr->map_cv == NULL) return NULL;

   mr->reduce_cv = malloc(threads * sizeof(pthread_cond_t));
   if(mr->reduce_cv == NULL) return NULL;

   for (int i=0; i<threads; i++) {
       pthread_mutex_init(&mr->_lock[i], NULL);
       pthread_cond_init(&mr->map_cv[i], NULL);
       pthread_cond_init(&mr->reduce_cv[i], NULL);
   }

   // buffer
   // kv_pair count
   mr->count = calloc(threads, sizeof(int));
   if(mr->count == NULL) return NULL;
   // size in bytes of kvpairs
   mr->size = calloc(threads, sizeof(int));
   if(mr->size == NULL) return NULL;

   //
   mr->HEAD = malloc(sizeof(struct buffer_node *) * threads);
   mr->TAIL = malloc(sizeof(struct buffer_node *) * threads);
   if(mr->HEAD == NULL || mr->TAIL == NULL) return NULL;


   // create a buffer list (can contain threads pointers)
   mr->buffer_list = malloc(sizeof(struct buffer_node*) * threads);
   if(mr->buffer_list != NULL) {
     for(int i=0; i<threads; i++){
       //buffer_list[i] = calloc(MR_BUFFER_SIZE, sizeof(char));
       mr->buffer_list[i] = malloc(MR_BUFFER_SIZE);

       mr->HEAD[i] = mr->TAIL[i] = mr->buffer_list[i];
       // make a cycle
       mr->HEAD[i]->next = mr->TAIL[i];
       mr->TAIL[i]->next = mr->HEAD[i];
     }
   }

	 return mr;
}

int
mr_start(struct map_reduce *mr, const char *inpath, const char *outpath) {

  struct args_helper *map_args,
             *reduce_args;

	for(int i=0; i<(mr->n_threads); i++) {   // Create n threads for map function (n = n_threads)

    mr->infd[i] = open(inpath, O_RDONLY, 644);  // assign different fd to every map thread
    if (mr->infd[i] ==  -1) return -1;

    map_args = &(mr->args[i]);
    map_args->mr = mr;
    map_args->map = mr->map;
    map_args->reduce = mr->reduce;
    map_args->infd = mr->infd[i];
    map_args->id = i;
    map_args->nmaps = mr->n_threads;

		mr->map_thread_failed[i] = pthread_create(&mr->map_threads[i], NULL, &map_wrapper, (void *)map_args);
    if (mr->map_thread_failed[i] != 0)
      return -1;
	}

  // Create a thread for reduce function

  mr->outfd = open(outpath, O_WRONLY | O_CREAT | O_TRUNC, 644);
  if (mr->outfd == -1) return -1;

  reduce_args = &(mr->args[mr->n_threads]);
  reduce_args->mr = mr;
  reduce_args->reduce = mr->reduce;
  reduce_args->map = mr->map;
  reduce_args->outfd = mr->outfd;
  reduce_args->nmaps = mr->n_threads;

	mr->reduce_thread_failed =  pthread_create(&mr->reduce_thread, NULL, &reduce_wrapper, (void *)reduce_args);
  if (mr->reduce_thread_failed != 0)
    return -1;

	return 0;
}

void
mr_destroy(struct map_reduce *mr) {
  for(int i=0; i<mr->n_threads; i++){
    free(mr->buffer_list[i]);
  }
  free(mr->buffer_list);
  free(mr->HEAD);
  free(mr->TAIL);
  free(mr->map_cv);
  free(mr->reduce_cv);
  free(mr->_lock);
  free(mr->count);
  free(mr->size);
  free(mr->infd_failed);
  free(mr->map_return_values);
  free(mr->map_thread_failed);
  free(mr->mapfn_failed);
  free(mr->map_threads);
  free(mr->infd);
  free(mr->args);
  free(mr);
}

int
mr_finish(struct map_reduce *mr) {

  void *reduce_return_value;
  // close threads
  for(int i=0; i<(mr->n_threads); i++) {
      if (mr->map_thread_failed[i] == 0) { //success
        if(pthread_join(mr->map_threads[i], &mr->map_return_values[i])) { // failed
          return -1;
        }
      pthread_cond_signal(&mr->reduce_cv[i]);
      }
  }

  if(mr->reduce_thread_failed == 0) // success
    if(pthread_join(mr->reduce_thread, &reduce_return_value)) // failed
      return -1;

  // close fd
  for(int i=0; i<(mr->n_threads); i++)
    mr->infd_failed[i] = close(mr->infd[i]);
  mr->outfd_failed = close(mr->outfd);

  // check if success
  if (mr->outfd_failed == -1 || mr->reducefn_failed != 0)
    return -1;

  for(int i=0; i<(mr->n_threads); i++){
      if (mr->infd_failed[i] == -1 || mr->mapfn_failed[i] != 0)
        return -1;  // failed
  }

  return 0; //success
  //check array
}

/**
 * Called by a Map thread each time it produces a key-value pair to be consumed
 * by the Reduce thread.
 * If the framework cannot currently store another
 * key-value pair, this function should block until it can.
 *
 * mr  Pointer to the MapReduce instance
 * id  Identifier of this Map thread, from 0 to (nmaps - 1)
 * kv  Pointer to the key-value pair that was produced by Map.  This pointer
 *     belongs to the caller, so you must copy the key and value data if you
 *     wish to store them somewhere.
 *
 * Returns 1 if one key-value pair is successfully produced (success), -1 on
 * failure.  (This convention mirrors that of the standard "write" function.)
 */
int
mr_produce(struct map_reduce *mr, int id, const struct kvpair *kv)
{
  if(kv == NULL) return -1;
  // get the kv_size
  int kv_size = kv->keysz + kv->valuesz + 2*sizeof(uint32_t);

  if(pthread_mutex_lock(&mr->_lock[id]) != 0) return -1; // lock failed

  // first check if the buffer is overflow
  while(mr->size[id] + kv_size >= MR_BUFFER_SIZE) {
    if(pthread_cond_wait(&mr->map_cv[id], &mr->_lock[id]) != 0) return -1; // wait failed
  }

  // allocate TAIL->next
  //mr->TAIL[id]->next = malloc(sizeof(struct buffer_node));
  struct buffer_node *new_node = mr->TAIL[id]->next;
  // allocate TAIL->next->kv
  //new_node->kv = malloc(kv_size);

  //if(new_node == NULL || new_node->kv == NULL) return -1;
  if(new_node == NULL) return -1;

  int addition = 0;
  memmove(&new_node->kv+addition, kv->key, kv->keysz);
  addition+=kv->keysz;
  memmove(&new_node->kv+addition, kv->value, kv->valuesz);
  addition+=kv->valuesz;
  memmove(&new_node->kv+addition, &kv->keysz, sizeof(uint32_t));
  addition+=sizeof(uint32_t);
  memmove(&new_node->kv+addition, &kv->valuesz, sizeof(uint32_t));

  new_node->keysz = kv->keysz;
  new_node->valuesz = kv->valuesz;

  // change tail->next to tail
  mr->TAIL[id] = new_node;
  mr->TAIL[id]->next = mr->HEAD[id];

  // add the size
  mr->size[id] += kv_size;
  mr->count[id]++;

  //printf("ID is %d, Count is %d, Valuesz is %d, value is %s\n", id, mr->count[id], mr->TAIL[id]->kv->valuesz, (char *)mr->TAIL[id]->kv->value);
  printf("ID is %d, Count is %d, new_node->valuesz is %d, mr->size[id] is %d\n", id, mr->count[id], new_node->valuesz, mr->size[id]);

  pthread_cond_signal (&mr->map_cv[id]);//from demo code
  if(pthread_mutex_unlock(&mr->_lock[id]) != 0) return -1; // unlock failed

	return 1; // successful
}

/**
 * Called by the Reduce function to consume a key-value pair from a given Map
 * thread.  If there is no key-value pair available, this function should block
 * until one is produced (in which case it will return 1) or the specified Map
 * thread returns (in which case it will return 0).
 *
 * mr  Pointer to the MapReduce instance
 * id  Identifier of Map thread from which to consume
 * kv  Pointer to the key-value pair that was produced by Map.  The caller is
 *     responsible for allocating memory for the key and value a of time and
 *     setting the pointer and size fields for each to the location and size of
 *     the allocated buffer.
 *
 * Returns 1 if one pair is successfully consumed, 0 if the Map thread returns
 * without producing any more pairs, or -1 on error.  (This convention mirrors
 * that of the standard "read" function.)
 */
int
mr_consume(struct map_reduce *mr, int id, struct kvpair *kv)
{
  if(kv == NULL) return -1;

  if(pthread_mutex_lock(&mr->_lock[id]) != 0) return -1; // lock failed
  //pthread_mutex_lock(&mr->_lock[id]);

  // make surew there is value to consume
  while(mr->count[id] <= 0 && (int)(intptr_t)mr->map_return_values[id] == -1) {
    if(pthread_cond_wait(&mr->reduce_cv[id], &mr->_lock[id]) != 0) return -1; // wait failed
    //pthread_cond_wait(&mr->reduce_cv[id], &mr->_lock[id]); // wait failed
  }

  if((int)(intptr_t)mr->map_return_values[id] == -1) return 0; // no more pairs
  printf("ID is %d, Count is %d, mr->HEAD[id]->valuesz is %d, mr->size[id] is %d\n", id, mr->count[id], mr->HEAD[id]->valuesz, mr->size[id]);

  // read from head
  int kv_size = 0;
  memmove(kv->key, &mr->HEAD[id]->kv+kv_size, mr->HEAD[id]->keysz);
  kv_size+=mr->HEAD[id]->keysz;
  memmove(kv->value, &mr->HEAD[id]->kv+kv_size, mr->HEAD[id]->valuesz);   //TODO
  kv_size+=mr->HEAD[id]->valuesz;
  memmove(&kv->keysz, &mr->HEAD[id]->kv+kv_size, sizeof(uint32_t));
  kv_size+=sizeof(uint32_t);
  memmove(&kv->valuesz, &mr->HEAD[id]->kv+kv_size, sizeof(uint32_t));
  kv_size+=sizeof(uint32_t);

  // remove head
  mr->HEAD[id] = mr->HEAD[id]->next;
  mr->TAIL[id]->next = mr->HEAD[id];

  // decrease size
  mr->size[id] -= kv_size;
  mr->count[id]--;

  pthread_cond_signal (&mr->reduce_cv[id]);//from demo code
  if(pthread_mutex_unlock(&mr->_lock[id]) != 0) return -1; // unlock failed

  printf("ID is %d, Count is %d, mr->size[id] is %d\n", id, mr->count[id], mr->size[id]);

	return 1; // successful
}
