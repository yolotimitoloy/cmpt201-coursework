#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define MY_SOCK_PATH "tmp"
#define BUF_SIZE 64

#define handle_error(msg)                                                      \
  do {                                                                         \
    perror(msg);                                                               \
    exit(EXIT_FAILURE);                                                        \
  } while (0)

int main() {
  int sfd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (sfd == -1)
    handle_error("socket");

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(struct sockaddr_un));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, MY_SOCK_PATH, sizeof(addr.sun_path) - 1);

  ssize_t num_read;
  char buf[BUF_SIZE];

  while ((num_read = read(STDIN_FILENO, buf, BUF_SIZE)) > 1)
    if (sendto(sfd, buf, num_read, 0, (struct sockaddr *)&addr,
               sizeof(struct sockaddr_un)) != num_read)
      handle_error("sendto");

  exit(EXIT_SUCCESS);
}
