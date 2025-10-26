#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ASSERT(expr)                                                           \
  {                                                                            \
    if (!(expr)) {                                                             \
      fprintf(stderr, "Assertion failed: %s\n", #expr);                        \
      exit(1);                                                                 \
    }                                                                          \
  }

#define TEST(expr)                                                             \
  {                                                                            \
    if (!(expr)) {                                                             \
      fprintf(stderr, "Test failed: %s\n", #expr);                             \
      exit(1);                                                                 \
    }                                                                          \
  }

typedef struct node {
  uint64_t data;
  struct node *next;
} node_t;

typedef struct info {
  uint64_t sum;
} info_t;

node_t *head = NULL;
info_t info = {0};

static uint64_t sum_list(void) {
  uint64_t s = 0;
  for (node_t *p = head; p != NULL; p = p->next) {
    s += p->data;
  }
  return s;
}

void insert_sorted(uint64_t data) {
  node_t *new_node = malloc(sizeof(node_t));
  ASSERT(new_node != NULL);
  new_node->data = data;
  new_node->next = NULL;

  if (head == NULL || data < head->data) {
    new_node->next = head;
    head = new_node;
    info.sum += data;
    return;
  }

  node_t *curr = head;
  node_t *prev = NULL;
  while (curr != NULL && curr->data < data) {
    prev = curr;
    curr = curr->next;
  }
  prev->next = new_node;
  new_node->next = curr;

  info.sum += data;
}

int index_of(uint64_t data) {
  node_t *curr = head;
  int index = 0;

  while (curr != NULL) {
    if (curr->data == data) {
      return index;
    }

    curr = curr->next;
    index++;
  }

  return -1;
}

int main() {
  insert_sorted(1);
  insert_sorted(3);
  insert_sorted(5);
  insert_sorted(2);
  TEST(index_of(2) == 1);

  uint64_t sum = sum_list();
  printf("info: %lu, sum: %lu\n", info.sum, sum);
  ASSERT(info.sum == sum);
  puts("example_2: ok");
  return 0;
}
