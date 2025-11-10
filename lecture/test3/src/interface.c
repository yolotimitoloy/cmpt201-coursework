///*
#include "interface.h"
#include "uthash.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_INTERMEDIATE 1024
#define MAX_VALUES_PER_KEY 256

// -----------------------------
// INTERMEDIATE STORAGE
// -----------------------------
static struct mr_out_kv intermediate[MAX_INTERMEDIATE];
static size_t intermediate_count = 0;
pthread_mutex_t intermediate_mutex = PTHREAD_MUTEX_INITIALIZER;

// -----------------------------
// FINAL STORAGE (with uthash)
// -----------------------------
struct final_kv {
  char key[MAX_KEY_SIZE];
  char (*value)[MAX_VALUE_SIZE];
  size_t count;
  UT_hash_handle hh;
};
static struct final_kv *final_table = NULL;
pthread_mutex_t final_mutex = PTHREAD_MUTEX_INITIALIZER;

// -----------------------------
// HELPER: INIT
// -----------------------------
static void init_intermediate() {
  for (size_t i = 0; i < MAX_INTERMEDIATE; i++) {
    intermediate[i].value =
        malloc(sizeof(char[MAX_VALUES_PER_KEY][MAX_VALUE_SIZE]));
    intermediate[i].count = 0;
    intermediate[i].key[0] = '\0';
  }
}

static void free_intermediate() {
  for (size_t i = 0; i < intermediate_count; i++) {
    free(intermediate[i].value);
  }
}

static void free_final_table() {
  struct final_kv *current, *tmp;
  HASH_ITER(hh, final_table, current, tmp) {
    HASH_DEL(final_table, current);
    free(current->value);
    free(current);
  }
}

// -----------------------------
// FIND OR CREATE INTERMEDIATE
// -----------------------------
static struct mr_out_kv *find_or_create_intermediate(const char *key) {
  for (size_t i = 0; i < intermediate_count; i++) {
    if (strcmp(intermediate[i].key, key) == 0)
      return &intermediate[i];
  }
  if (intermediate_count >= MAX_INTERMEDIATE)
    return NULL;
  struct mr_out_kv *kv = &intermediate[intermediate_count++];
  strcpy(kv->key, key);
  kv->count = 0;
  return kv;
}

// -----------------------------
// EMIT FUNCTIONS
// -----------------------------
int mr_emit_i(const char *key, const char *value) {
  pthread_mutex_lock(&intermediate_mutex);
  struct mr_out_kv *kv = find_or_create_intermediate(key);
  if (!kv) {
    pthread_mutex_unlock(&intermediate_mutex);
    return -1;
  }
  strcpy(kv->value[kv->count], value);
  kv->count++;
  pthread_mutex_unlock(&intermediate_mutex);
  return 0;
}

int mr_emit_f(const char *key, const char *value) {
  pthread_mutex_lock(&final_mutex);
  struct final_kv *entry = NULL;
  HASH_FIND_STR(final_table, key, entry);
  if (!entry) {
    entry = malloc(sizeof(struct final_kv));
    strcpy(entry->key, key);
    entry->count = 0;
    entry->value = malloc(sizeof(char[MAX_VALUES_PER_KEY][MAX_VALUE_SIZE]));
    HASH_ADD_STR(final_table, key, entry);
  }
  strcpy(entry->value[entry->count], value);
  entry->count++;
  pthread_mutex_unlock(&final_mutex);
  return 0;
}

// -----------------------------
// THREAD ARGUMENTS
// -----------------------------
struct map_args {
  const struct mr_in_kv *input;
  size_t start;
  size_t end;
  void (*map)(const struct mr_in_kv *);
};

struct reduce_args {
  size_t start;
  size_t end;
  void (*reduce)(const struct mr_out_kv *);
};

// -----------------------------
// THREAD FUNCTIONS
// -----------------------------
void *map_thread(void *arg) {
  struct map_args *args = arg;
  for (size_t i = args->start; i < args->end; i++) {
    args->map(&args->input[i]);
  }
  return NULL;
}

void *reduce_thread(void *arg) {
  struct reduce_args *args = arg;
  for (size_t i = args->start; i < args->end; i++) {
    args->reduce(&intermediate[i]);
  }
  return NULL;
}

int final_kv_cmp(struct final_kv *a, struct final_kv *b) {
  return strcmp(a->key, b->key);
}

// -----------------------------
// EXECUTE MAPREDUCE
// -----------------------------
int mr_exec(const struct mr_input *input, void (*map)(const struct mr_in_kv *),
            size_t mapper_count, void (*reduce)(const struct mr_out_kv *),
            size_t reducer_count, struct mr_output *output) {

  init_intermediate();
  intermediate_count = 0;

  // -------------------------
  // MAP PHASE
  // -------------------------
  pthread_t mthreads[mapper_count];
  struct map_args margs[mapper_count];
  size_t chunk_size = (input->count + mapper_count - 1) / mapper_count;

  for (size_t t = 0; t < mapper_count; t++) {
    margs[t].input = input->kv_lst;
    margs[t].start = t * chunk_size;
    margs[t].end = (t + 1) * chunk_size;
    if (margs[t].end > input->count)
      margs[t].end = input->count;
    margs[t].map = map;
    pthread_create(&mthreads[t], NULL, map_thread, &margs[t]);
  }

  for (size_t t = 0; t < mapper_count; t++)
    pthread_join(mthreads[t], NULL);

  // -------------------------
  // REDUCE PHASE
  // -------------------------
  pthread_t rthreads[reducer_count];
  struct reduce_args rargs[reducer_count];
  size_t rchunk = (intermediate_count + reducer_count - 1) / reducer_count;

  for (size_t t = 0; t < reducer_count; t++) {
    rargs[t].start = t * rchunk;
    rargs[t].end = (t + 1) * rchunk;
    if (rargs[t].end > intermediate_count)
      rargs[t].end = intermediate_count;
    rargs[t].reduce = reduce;
    pthread_create(&rthreads[t], NULL, reduce_thread, &rargs[t]);
  }

  for (size_t t = 0; t < reducer_count; t++)
    pthread_join(rthreads[t], NULL);

  // -------------------------
  // WRITE FINAL OUTPUT
  // -------------------------

  // Sort the hash table by key
  HASH_SORT(final_table, final_kv_cmp);

  // Count entries
  size_t count = HASH_COUNT(final_table);
  output->kv_lst = malloc(sizeof(struct mr_out_kv) * count);
  output->count = 0;

  // Copy sorted entries to output
  struct final_kv *entry, *tmp;
  HASH_ITER(hh, final_table, entry, tmp) {
    struct mr_out_kv *out = &output->kv_lst[output->count];
    strcpy(out->key, entry->key);
    out->count = entry->count;
    out->value = malloc(sizeof(char[MAX_VALUES_PER_KEY][MAX_VALUE_SIZE]));
    for (size_t i = 0; i < entry->count; i++) {
      strcpy(out->value[i], entry->value[i]);
    }
    output->count++;
  }

  // cleanup
  // free_intermediate();
  // free_final_table();

  return 0;
}
//*/
/*
#include "interface.h"
#include "uthash.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

pthread_mutex_t in_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t out_mutex = PTHREAD_MUTEX_INITIALIZER;

struct out_kv_hash {
  char key[256];
  char (*value)[256];
  size_t count;
  UT_hash_handle hh;
};

struct map_args {
  const struct mr_in_kv *input;
  size_t start;
  size_t end;
  void (*map)(const struct mr_in_kv *);
};

struct reduce_args {
  size_t start;
  size_t end;
  void (*reduce)(const struct mr_out_kv *);
};

static struct out_kv_hash *out_hash = NULL;
static size_t i_count = 0;
static struct mr_out_kv intermediate[4096];

void *mapperThread(void *args) {
  struct map_args *temp = args;
  for (size_t i = temp->start; i < temp->end; i++) {
    temp->map(&temp->input[i]);
  }
  return NULL;
}

void *reducerThread(void *args) {
  struct reduce_args *temp = args;
  for (size_t i = temp->start; i < temp->end; i++) {
    temp->reduce(&intermediate[i]);
  }
  return NULL;
}

int out_sort(struct out_kv_hash *a, struct out_kv_hash *b) {
  return strcmp(a->key, b->key);
}

static struct mr_out_kv *find_or_create(const char *key) {
  for (size_t i = 0; i < i_count; i++) {
    if (strcmp(intermediate[i].key, key) == 0) {
      return &intermediate[i];
    }
  }
  if (i_count >= 4096) {
    return NULL;
  }
  struct mr_out_kv *temp = &intermediate[i_count++];
  strcpy(temp->key, key);
  temp->count = 0;
  return temp;
}

int mr_emit_i(const char *key, const char *value) {
  pthread_mutex_lock(&in_mutex);
  struct mr_out_kv *kv = find_or_create(key);
  if (kv == NULL) {
    pthread_mutex_unlock(&in_mutex);
    return -1;
  }
  strcpy(kv->value[kv->count], value);
  kv->count++;
  pthread_mutex_unlock(&in_mutex);
  return 0;
}
int mr_emit_f(const char *key, const char *value) {
  pthread_mutex_lock(&out_mutex);
  struct out_kv_hash *temp = NULL;
  HASH_FIND_STR(out_hash, key, temp);
  if (temp == NULL) {
    temp = malloc(sizeof(struct out_kv_hash));
    strcpy(temp->key, key);
    temp->count = 0;
    temp->value = malloc(sizeof(char[256][256]));
    HASH_ADD_STR(out_hash, key, temp);
  }
  strcpy(temp->value[temp->count], value);
  temp->count++;
  pthread_mutex_unlock(&out_mutex);
  return 0;
}

int mr_exec(const struct mr_input *input, void (*map)(const struct mr_in_kv *),
            size_t mapper_count, void (*reduce)(const struct mr_out_kv *),
            size_t reducer_count, struct mr_output *output) {

  for (size_t i = 0; i < 4096; i++) {
    intermediate[i].value = malloc(sizeof(char[256][256]));
    intermediate[i].count = 0;
    intermediate[i].key[0] = '\0';
  }
  i_count = 0;
  pthread_t mapthread[mapper_count];
  size_t chunk_size_map = (input->count + mapper_count - 1) / mapper_count;
  struct map_args mapTemp[mapper_count];
  for (size_t i = 0; i < mapper_count; i++) {
    mapTemp[i].input = input->kv_lst;
    mapTemp[i].start = i * chunk_size_map;
    mapTemp[i].end = (i + 1) * chunk_size_map;
    mapTemp[i].map = map;
    pthread_create(&mapthread[i], NULL, mapperThread, &mapTemp[i]);
  }

  for (size_t i = 0; i < mapper_count; i++) {
    pthread_join(mapthread[i], NULL);
  }

  pthread_t reducethread[reducer_count];
  size_t chunk_size_reduce = (input->count + reducer_count - 1) / reducer_count;
  struct reduce_args reduceTemp[reducer_count];
  for (size_t i = 0; i < reducer_count; i++) {
    reduceTemp[i].start = i * chunk_size_reduce;
    reduceTemp[i].end = (i + 1) * chunk_size_reduce;
    reduceTemp[i].reduce = reduce;
    pthread_create(&reducethread[i], NULL, reducerThread, &reduceTemp[i]);
  }
  for (size_t i = 0; i < reducer_count; i++) {
    pthread_join(reducethread[i], NULL);
  }

  HASH_SORT(out_hash, out_sort);
  size_t count = HASH_COUNT(out_hash);
  output->kv_lst = malloc(sizeof(struct mr_out_kv) * count);
  output->count = 0;

  struct out_kv_hash *temp1;
  struct out_kv_hash *temp2;
  HASH_ITER(hh, out_hash, temp1, temp2) {
    struct mr_out_kv *out = &output->kv_lst[output->count];
    strcpy(out->key, temp1->key);
    out->count = temp1->count;
    out->value = malloc(sizeof(char[256][256]));
    for (size_t i = 0; i < temp1->count; i++) {
      strcpy(out->value[i], temp1->value[i]);
    }
    output->count++;
  }

  return 0;
}
*/
