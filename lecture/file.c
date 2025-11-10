#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int main() {
  char *str = "YOLO!!!!\n";
  char buf[12];
  int fd = open("tmp", O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  if (fd == -1) {
    perror("can't open file");
    exit(1);
  }

  write(fd, str, strlen(str));

  for (;;) {
    sleep(30);
  }
  // lseek(fd, -6, SEEK_CUR);
  // read(fd, buf, 6);
  //  write(STDOUT_FILENO, buf, 6);
}
