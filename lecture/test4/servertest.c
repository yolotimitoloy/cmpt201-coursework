#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// --- Constants ---
#define MAX_MSG_SIZE 1024
#define MAX_CLIENTS 100
#define TYPE_REGULAR 0
#define TYPE_END_EXECUTION 1

// --- Global Data Structures for Message Queue ---

typedef struct message_node {
  char buffer[MAX_MSG_SIZE]; // Fully constructed broadcast message
  size_t len;
  struct message_node *next;
} message_node_t;

message_node_t *queue_head = NULL;
message_node_t *queue_tail = NULL;

// --- Global Data Structures for Clients ---

// Note: This structure now only holds data needed for broadcast/cleanup
typedef struct {
  int fd;
  pthread_t io_tid; // Thread ID of the dedicated I/O thread for this client
  struct sockaddr_in addr;
  socklen_t addr_len;
  // Note: The read buffer and client run flag are now local to the I/O thread
} client_t;

client_t clients[MAX_CLIENTS];
int num_clients = 0;
int expected_clients = 0;
int finished_clients_count = 0;

// --- Synchronization Primitives ---

pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t client_list_lock = PTHREAD_MUTEX_INITIALIZER;
volatile int server_running = 1; // Flag for graceful shutdown

// --- Thread Arguments ---

// Arguments passed to the dedicated client I/O thread
typedef struct {
  int client_fd;
  int client_index; // Index in the global clients array for cleanup reference
} client_io_args_t;

// Arguments passed to the acceptor thread
typedef struct {
  int listen_fd;
  pthread_t *worker_tid; // Reference to broadcast worker TID for cleanup
} acceptor_args_t;

// --- Function Prototypes ---
void error(const char *msg);
void set_non_blocking(int fd);
void enqueue_message(const char *buffer, size_t len);
message_node_t *dequeue_message();
void *broadcast_worker(void *arg);
int handle_message(int sender_fd, char *buffer, ssize_t len, int listen_fd);
void remove_client(int fd);
void *run_client_io(void *arg);
void *run_acceptor(void *arg);
void cleanup_and_exit(int server_fd, pthread_t worker_tid,
                      pthread_t acceptor_tid);

/**
 * @brief Utility function to print error message and exit.
 */
void error(const char *msg) {
  perror(msg);
  exit(EXIT_FAILURE);
}

/**
 * @brief Sets a file descriptor to non-blocking mode.
 */
void set_non_blocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    error("fcntl F_GETFL");
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    error("fcntl F_SETFL");
  }
}

// --- Queue Operations (Thread-Safe) ---

/**
 * @brief Adds a message to the broadcast queue.
 */
void enqueue_message(const char *buffer, size_t len) {
  if (len > MAX_MSG_SIZE) {
    fprintf(stderr, "Enqueue failed: message length exceeds buffer size.\n");
    return;
  }

  message_node_t *new_node = (message_node_t *)malloc(sizeof(message_node_t));
  if (!new_node) {
    error("malloc failed for message node");
  }
  memcpy(new_node->buffer, buffer, len);
  new_node->len = len;
  new_node->next = NULL;

  pthread_mutex_lock(&queue_lock);
  if (queue_tail == NULL) {
    queue_head = new_node;
    queue_tail = new_node;
  } else {
    queue_tail->next = new_node;
    queue_tail = new_node;
  }

  // Signal the broadcast worker that a new message is ready
  pthread_cond_signal(&queue_cond);
  pthread_mutex_unlock(&queue_lock);
}

/**
 * @brief Retrieves a message from the broadcast queue.
 */
message_node_t *dequeue_message() {
  pthread_mutex_lock(&queue_lock);

  // Wait until queue is not empty or server is shutting down
  while (queue_head == NULL && server_running) {
    pthread_cond_wait(&queue_cond, &queue_lock);
  }

  if (queue_head == NULL) {
    // If queue is empty AND server_running is 0, this is the exit path
    pthread_mutex_unlock(&queue_lock);
    return NULL;
  }

  message_node_t *node = queue_head;
  queue_head = queue_head->next;
  if (queue_head == NULL) {
    queue_tail = NULL;
  }

  pthread_mutex_unlock(&queue_lock);
  return node;
}

// --- Client Management ---

/**
 * @brief Removes a client from the tracking array. (Protected by
 * client_list_lock)
 */
void remove_client(int fd) {
  pthread_mutex_lock(&client_list_lock);
  for (int i = 0; i < num_clients; i++) {
    if (clients[i].fd == fd) {
      // The I/O thread has already closed the FD, just clean up the array
      // Move last client into the freed slot and decrease count
      if (i < num_clients - 1) {
        clients[i] = clients[num_clients - 1];
      }
      num_clients--;
      printf("Client disconnected (FD %d). Current clients: %d\n", fd,
             num_clients);
      pthread_mutex_unlock(&client_list_lock);
      return;
    }
  }
  pthread_mutex_unlock(&client_list_lock);
}

/**
 * @brief Handles one complete message read from a client.
 * NOTE: The client I/O thread should acquire client_list_lock before calling
 * this if modifying finished_clients_count.
 * @return >0 if message was processed (length processed), 0 if incomplete, -1
 * on error/disconnect, -2 on server shutdown.
 */
int handle_message(int sender_fd, char *buffer, ssize_t len, int listen_fd) {
  if (len <= 0) {
    return -1; // Should not happen here, but safety check
  }

  int shutdown_triggered = 0;
  size_t msg_len = 0;

  // Check for a full message ending with '\n'
  char *newline_pos = memchr(buffer, '\n', len);
  if (!newline_pos) {
    return 0; // Incomplete message
  }

  msg_len = newline_pos - buffer + 1; // Length including '\n'
  uint8_t type = (uint8_t)buffer[0];

  if (type == TYPE_REGULAR) {
    // --- Type 0: Regular Group Chat Message ---

    struct sockaddr_in sender_addr;
    socklen_t addr_len = sizeof(sender_addr);
    // Note: getpeername is fine as it doesn't modify state requiring a lock
    if (getpeername(sender_fd, (struct sockaddr *)&sender_addr, &addr_len) ==
        -1) {
      perror("getpeername failed");
    }

    uint32_t ip_nbo = sender_addr.sin_addr.s_addr;
    uint16_t port_nbo = sender_addr.sin_port;

    char broadcast_buffer[MAX_MSG_SIZE];

    if (msg_len + 6 > MAX_MSG_SIZE) {
      fprintf(stderr, "Message too long to broadcast. Dropping.\n");
    } else {
      // Build the broadcast message buffer
      broadcast_buffer[0] = TYPE_REGULAR;
      memcpy(broadcast_buffer + 1, &ip_nbo, sizeof(ip_nbo));
      memcpy(broadcast_buffer + 1 + sizeof(ip_nbo), &port_nbo,
             sizeof(port_nbo));
      memcpy(broadcast_buffer + 7, buffer + 1, msg_len - 1);

      size_t broadcast_len = msg_len + 6;
      enqueue_message(broadcast_buffer, broadcast_len);
    }

  } else if (type == TYPE_END_EXECUTION) {
    // --- Type 1: Termination Signal (Phase 1) ---

    if (msg_len != 2) {
      fprintf(stderr,
              "Received incorrect Type 1 message format (len=%zu). Must be 2 "
              "bytes. Discarding.\n",
              msg_len);
    } else {
      // *** Acquire Lock to modify shared state ***
      pthread_mutex_lock(&client_list_lock);
      finished_clients_count++;
      printf("--- T1 RECEIVED --- FD %d. Count: %d/%d\n", sender_fd,
             finished_clients_count, expected_clients);

      if (finished_clients_count == expected_clients) {
        // --- Phase 2: Commit ---
        printf("All expected clients finished. Starting Two-Phase Commit "
               "(Phase 2).\n");

        char commit_msg[] = {TYPE_END_EXECUTION, '\n'};
        enqueue_message(commit_msg, sizeof(commit_msg));

        // Set flag for all other threads to exit
        server_running = 0;
        shutdown_triggered = 1;
        pthread_cond_signal(&queue_cond); // Wake up broadcast worker
      }
      pthread_mutex_unlock(&client_list_lock);
    }
  } else {
    fprintf(stderr, "Received unknown message type: %u. Discarding message.\n",
            type);
  }

  // Shift the remaining data in the buffer (if any) to the start
  if (len > msg_len) {
    memmove(buffer, buffer + msg_len, len - msg_len);
  }

  // If termination was initiated, return the special exit code
  if (shutdown_triggered) {
    return -2;
  }

  return msg_len;
}

// --- Thread Functions ---

/**
 * @brief Thread routine for handling a single client's non-blocking I/O.
 * It busy-polls the socket until a disconnect or shutdown.
 */
void *run_client_io(void *arg) {
  client_io_args_t *cargs = (client_io_args_t *)arg;
  int client_fd = cargs->client_fd;

  // Local buffer for this client thread
  char buffer[MAX_MSG_SIZE];
  ssize_t data_len = 0;

  // Set the socket to non-blocking
  set_non_blocking(client_fd);

  while (server_running) {
    // Attempt to read data into the current buffer space
    ssize_t bytes_read =
        read(client_fd, buffer + data_len, MAX_MSG_SIZE - data_len);

    if (bytes_read == -1) {
      if (!(errno == EAGAIN || errno == EWOULDBLOCK)) {
        perror("Client I/O: Permanent read error");
        break; // Exit loop on error
      }
    } else if (bytes_read == 0) {
      // Connection closed by client
      printf("Client I/O: Client FD %d gracefully closed connection.\n",
             client_fd);
      break;
    } else {
      // Data received
      data_len += bytes_read;

      // Process all full messages that arrived in the buffer
      int process_result = 0;
      while ((data_len > 0) && (process_result = handle_message(
                                    client_fd, buffer, data_len, 0)) > 0) {
        data_len -= process_result;
      }

      if (process_result == -2) {
        // Server termination signal received (Phase 2 completed)
        free(cargs);
        // Note: Closing FD here will cause the acceptor to hang on join if not
        // handled correctly. The cleanup_and_exit handles closing all FDs.
        return NULL;
      } else if (process_result < 0) {
        // Error during message parsing or unexpected disconnect during read
        fprintf(stderr,
                "Error processing message from FD %d, forcing disconnect.\n",
                client_fd);
        break; // Exit loop on parsing error
      }
    }

    // Non-blocking poll: yield execution for a short period to prevent
    // excessive CPU usage from busy-waiting
    usleep(50000); // 50 milliseconds
  }

  // Cleanup upon thread exit (error, disconnect, or local break)
  if (close(client_fd) == -1) {
    perror("Error closing client FD");
  }
  remove_client(client_fd);
  free(cargs);
  return NULL;
}

/**
 * @brief Worker thread that dequeues messages and sends them to all clients.
 */
void *broadcast_worker(void *arg) {
  message_node_t *node;

  while (server_running) {
    node = dequeue_message();

    if (node == NULL) {
      if (!server_running)
        break;
      continue;
    }

    // --- Broadcast Message (Type 0 or Type 1) ---
    pthread_mutex_lock(&client_list_lock);

    if ((uint8_t)node->buffer[0] == TYPE_END_EXECUTION) {
      printf("Broadcast Worker: Sending Phase 2 commit to all %d clients.\n",
             num_clients);
    }

    // Iterate through the client list and send the message
    for (int i = 0; i < num_clients; i++) {
      if (write(clients[i].fd, node->buffer, node->len) == -1) {
        perror("Broadcast Worker: Error writing broadcast message to client");
        // We rely on the client's I/O thread to detect the write error via a
        // subsequent read error/disconnect, or let the main cleanup handle it.
      }
    }

    pthread_mutex_unlock(&client_list_lock);

    free(node);
  }
  printf("Broadcast Worker thread exiting.\n");
  return NULL;
}

/**
 * @brief Dedicated thread for accepting new connections.
 */
void *run_acceptor(void *arg) {
  acceptor_args_t *aargs = (acceptor_args_t *)arg;
  int listen_fd = aargs->listen_fd;
  int new_fd;
  struct sockaddr_in cli_addr;
  socklen_t clilen;

  // Set the listener to non-blocking to allow the thread to check
  // server_running flag periodically (prevents blocking indefinitely on accept)
  set_non_blocking(listen_fd);

  while (server_running) {
    clilen = sizeof(cli_addr);
    new_fd = accept(listen_fd, (struct sockaddr *)&cli_addr, &clilen);

    if (new_fd < 0) {
      if (!(errno == EWOULDBLOCK || errno == EAGAIN)) {
        perror("accept error");
        // Major error, break and signal shutdown
        server_running = 0;
        break;
      }
      // No connection ready, wait a bit before retrying
      usleep(100000); // 100 milliseconds
      continue;
    }

    // Connection accepted
    pthread_mutex_lock(&client_list_lock);
    if (num_clients < MAX_CLIENTS) {
      // 1. Add client to global list
      clients[num_clients].fd = new_fd;
      clients[num_clients].addr = cli_addr;
      clients[num_clients].addr_len = clilen;

      // 2. Prepare arguments for the I/O thread
      client_io_args_t *cargs =
          (client_io_args_t *)malloc(sizeof(client_io_args_t));
      if (!cargs)
        error("malloc client_io_args_t");

      cargs->client_fd = new_fd;
      cargs->client_index = num_clients;

      // 3. Launch I/O thread
      if (pthread_create(&clients[num_clients].io_tid, NULL, run_client_io,
                         cargs) != 0) {
        // Failed to create thread
        fprintf(stderr, "Error creating client I/O thread for FD %d.\n",
                new_fd);
        close(new_fd);
        free(cargs);
      } else {
        printf("New connection from %s:%d on FD %d. Clients: %d\n",
               inet_ntoa(cli_addr.sin_addr), ntohs(cli_addr.sin_port), new_fd,
               num_clients + 1);
        num_clients++;
      }
    } else {
      fprintf(stderr, "Connection rejected: Max clients reached.\n");
      close(new_fd);
    }
    pthread_mutex_unlock(&client_list_lock);
  }

  printf("Acceptor thread exiting.\n");
  return NULL;
}

/**
 * @brief Handles the final cleanup of all server resources.
 */
void cleanup_and_exit(int server_fd, pthread_t worker_tid,
                      pthread_t acceptor_tid) {

  printf("\nSERVER CLEANUP: Starting shutdown process...\n");

  // 1. Set running flag to 0 and wake up waiting threads
  server_running = 0;
  pthread_mutex_lock(&queue_lock);
  pthread_cond_signal(&queue_cond);
  pthread_mutex_unlock(&queue_lock);

  // 2. Join acceptor thread
  if (acceptor_tid != 0) {
    pthread_join(acceptor_tid, NULL);
  }

  // 3. Join all client I/O threads
  pthread_mutex_lock(&client_list_lock);
  for (int i = 0; i < num_clients; i++) {
    printf("Joining client I/O thread for FD %d\n", clients[i].fd);
    // Note: The client I/O thread closes its own FD.
    pthread_join(clients[i].io_tid, NULL);
  }
  num_clients = 0; // Reset count
  pthread_mutex_unlock(&client_list_lock);

  // 4. Join broadcast worker thread
  if (worker_tid != 0) {
    pthread_join(worker_tid, NULL);
  }

  // 5. Close listener socket
  if (server_fd != -1) {
    close(server_fd);
  }

  // 6. Clean up queue nodes
  message_node_t *current = queue_head;
  while (current != NULL) {
    message_node_t *next = current->next;
    free(current);
    current = next;
  }

  printf("Server shutting down gracefully.\n");
  exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[]) {
  int listen_fd = -1;
  struct sockaddr_in serv_addr;
  pthread_t worker_tid = 0;
  pthread_t acceptor_tid = 0;

  if (argc < 3) {
    fprintf(stderr, "Usage: %s <port number> <# of clients>\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  int port = atoi(argv[1]);
  expected_clients = atoi(argv[2]);

  if (expected_clients <= 0 || expected_clients > MAX_CLIENTS) {
    fprintf(stderr, "Error: Number of clients must be between 1 and %d.\n",
            MAX_CLIENTS);
    exit(EXIT_FAILURE);
  }

  // 1. Create and configure socket
  listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    error("ERROR opening socket");
  }

  int opt = 1;
  if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) ==
      -1) {
    perror("setsockopt");
  }

  // 2. Initialize and bind socket
  memset((char *)&serv_addr, 0, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = INADDR_ANY;
  serv_addr.sin_port = htons(port);

  if (bind(listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
    error("ERROR on binding");
  }

  // 3. Listen
  if (listen(listen_fd, MAX_CLIENTS) < 0) {
    error("ERROR on listen");
  }

  printf("Server listening on port %d, expecting %d clients...\n", port,
         expected_clients);

  // 4. Start the dedicated Broadcast Worker thread
  if (pthread_create(&worker_tid, NULL, broadcast_worker, NULL) != 0) {
    close(listen_fd);
    error("ERROR creating broadcast worker thread");
  }

  // 5. Start the dedicated Acceptor thread
  acceptor_args_t aargs = {
      .listen_fd = listen_fd,
      .worker_tid = &worker_tid,
  };
  if (pthread_create(&acceptor_tid, NULL, run_acceptor, &aargs) != 0) {
    // Attempt graceful shutdown
    server_running = 0;
    pthread_join(worker_tid, NULL);
    close(listen_fd);
    error("ERROR creating acceptor thread");
  }

  // 6. Main thread waits for the server_running flag to be set to 0 (by Type 1
  // commit)
  while (server_running) {
    // The main thread's only job is to stay alive until cleanup is triggered
    sleep(1);
  }

  // 7. Cleanup is initiated after the T1 commit sets server_running = 0
  cleanup_and_exit(listen_fd, worker_tid, acceptor_tid);
  return 0;
}
