#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define BUF_SIZE 64
#define MY_SOCK_PATH "tmp"
#define LISTEN_BACKLOG 32

#define handle_error(msg)                                                      \
  do {                                                                         \
    perror(msg);                                                               \
    exit(EXIT_FAILURE);                                                        \
  } while (0)

int main() {
  int sfd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (sfd == -1)
    handle_error("socket");

  if (remove(MY_SOCK_PATH) == -1 &&
      errno != ENOENT) // No such file or directory
    handle_error("remove");

  struct sockaddr_un addr;
  socklen_t len = sizeof(struct sockaddr_un);

  memset(&addr, 0, sizeof(struct sockaddr_un));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, MY_SOCK_PATH, sizeof(addr.sun_path) - 1);

  if (bind(sfd, (struct sockaddr *)&addr, sizeof(struct sockaddr_un)) == -1)
    handle_error("bind");

  for (;;) {
    char buf[BUF_SIZE];
    int num_read = recvfrom(sfd, buf, BUF_SIZE, 0, NULL, &len);
    if (write(STDOUT_FILENO, buf, num_read) != num_read)
      handle_error("write");
  }
}
