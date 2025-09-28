#include <stdio.h>
#include <unistd.h>

int main() {
  // int pid = fork();

  // if (pid == fork()) {
  // printf("we have not started:\n");
  // printf("current process ppid = %d \n", getppid());
  // printf("current process pid = %d \n", getpid());
  // fork();
  int temp = fork();
  printf("waiting 5 second.\n");

  // sleep(5);

  if (temp != 0) {
    printf("we are in parent process:\n");
    //    printf("parent done. ppid = %d \n", getpid());
    sleep(2);
    // printf("child done. pid = %d \n", temp);
    execlp("/bin/ls", "/bin/ls", "-a", (char *)NULL);
  } else {

    printf("we are in child process:\n");
    sleep(10);
    // printf("parent done. ppid = %d \n", getppid());
    execlp("/bin/ls", "/bin/ls", "-alh", (char *)NULL);
    printf("child done. pid = %d \n", getpid());
  }
  sleep(3);
  printf("waiting 3 second.\n");
}
