// this is the server code
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
int main() {
  printf("SERVER:\n");

  // socket
  int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (socket_fd == -1) {
    perror("socket failed!");
    exit(EXIT_FAILURE);
  }

  if (remove("socket_fun") == -1 && errno != ENOENT) {
    perror("remove failed!");
    exit(EXIT_FAILURE);
  }

  // bind
  struct sockaddr_un sockstruct;
  sockstruct.sun_family = AF_UNIX;
  snprintf(sockstruct.sun_path, 108, "socket_fun");

  if (bind(socket_fd, (struct sockaddr *)&sockstruct,
           sizeof(struct sockaddr_un)) == -1) {
    perror("bind failed!");
    exit(EXIT_FAILURE);
  }
  // listen
  if (listen(socket_fd, 10)) {
    perror("listen failed");
    exit(EXIT_FAILURE);
  }
  // accept
  int connected_fd = accept(socket_fd, NULL, NULL);
  if (connected_fd == -1) {
    perror("accept");
    exit(EXIT_FAILURE);
  }
  // read
  const int SIZE = 1024;
  char buff[SIZE];
  int byte_read = read(connected_fd, buff, SIZE);
  if (byte_read == -1) {
    perror("read");
    exit(EXIT_FAILURE);
  }

  // write
  write(STDOUT_FILENO, buff, byte_read);

  // close
  close(connected_fd);
  close(socket_fd);

  return 0;
}
