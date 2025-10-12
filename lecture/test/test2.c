#define _GNU_SOURCE
#include "msgs.h"
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_INPUT 4096
#define MAX_ARGS 16
#define HIST_SIZE 10

// ------------------------- Globals -------------------------
char *input_hist[HIST_SIZE];
int hist_count = 0; // total commands entered
int hist_size = 0;  // current stored in circular buffer

// -------------------- Signal Handler ----------------------
void sigint_handler(int sig) {
  // Only async-signal-safe functions here
  write(STDOUT_FILENO, "\n", 1);
  const char *msgs[] = {
      FORMAT_MSG("help", HELP_HELP_MSG), FORMAT_MSG("cd", CD_HELP_MSG),
      FORMAT_MSG("exit", EXIT_HELP_MSG), FORMAT_MSG("pwd", PWD_HELP_MSG),
      FORMAT_MSG("history", HISTORY_HELP_MSG)};
  for (int i = 0; i < 5; i++) {
    write(STDOUT_FILENO, msgs[i], strlen(msgs[i]));
  }
}

// -------------------- History Functions -------------------
void enqueue_history(const char *input) {
  if (hist_size == HIST_SIZE) {
    free(input_hist[0]);
    for (int i = 1; i < HIST_SIZE; i++)
      input_hist[i - 1] = input_hist[i];
    hist_size--;
  }
  input_hist[hist_size++] = strdup(input);
  hist_count++;
}

char *get_last_history() {
  return (hist_size > 0) ? input_hist[hist_size - 1] : NULL;
}

char *get_history(int n) {
  if (n <= 0 || n > hist_count)
    return NULL;
  int index =
      (hist_count > HIST_SIZE) ? (n - (hist_count - HIST_SIZE) - 1) : (n - 1);
  if (index < 0 || index >= hist_size)
    return NULL;
  return input_hist[index];
}

void print_history() {
  for (int i = 0; i < hist_size; i++) {
    int num = hist_count - hist_size + i + 1;
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%d: %s\n", num, input_hist[i]);
    write(STDOUT_FILENO, buf, len);
  }
}

// -------------------- Helper Functions --------------------
bool is_number_only(const char *s) {
  for (int i = 0; s[i]; i++)
    if (!isdigit((unsigned char)s[i]))
      return false;
  return true;
}

void tokenize(char *input, char **args, int max_args) {
  int i = 0;
  char *token = strtok(input, " \t\n");
  while (token && i < max_args - 1) {
    args[i++] = token;
    token = strtok(NULL, " \t\n");
  }
  args[i] = NULL;
}

bool is_background(char **args, int argc) {
  if (argc > 0 && strcmp(args[argc - 1], "&") == 0) {
    args[argc - 1] = NULL;
    return true;
  }
  return false;
}

void reap_background() {
  int status;
  pid_t pid;
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "[Reaped background PID %d]\n", pid);
    write(STDOUT_FILENO, buf, len);
  }
}

// ---------------------- Main Shell ------------------------
int main() {
  char input[MAX_INPUT];
  char cwd_buffer[4096];
  char lastPath[4096] = "";
  char *args[MAX_ARGS];
  int status;

  // Set up SIGINT
  struct sigaction sa;
  sa.sa_handler = sigint_handler;
  sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);

  while (1) {
    reap_background();

    // Prompt
    getcwd(cwd_buffer, sizeof(cwd_buffer));
    write(STDOUT_FILENO, cwd_buffer, strlen(cwd_buffer));
    write(STDOUT_FILENO, "$ ", 2);

    // Read input
    ssize_t n = read(STDIN_FILENO, input, sizeof(input) - 1);
    if (n <= 0)
      continue; // handle EINTR automatically
    input[n] = '\0';
    if (input[n - 1] == '\n')
      input[n - 1] = '\0';
    if (strlen(input) == 0)
      continue;

    // Handle !! and !n
    if (input[0] == '!') {
      if (input[1] == '!') {
        char *last = get_last_history();
        if (!last)
          continue;
        strncpy(input, last, sizeof(input));
      } else {
        if (!is_number_only(input + 1))
          continue;
        int n = atoi(input + 1);
        char *cmd = get_history(n);
        if (!cmd)
          continue;
        strncpy(input, cmd, sizeof(input));
      }
    }

    // Store in history
    enqueue_history(input);

    // Tokenize
    tokenize(input, args, MAX_ARGS);
    if (!args[0])
      continue;

    int argc = 0;
    while (args[argc])
      argc++;

    bool bg = is_background(args, argc);

    // -------------------- Internal Commands --------------------
    if (strcmp(args[0], "exit") == 0) {
      if (args[1]) {
        const char *msg = FORMAT_MSG("exit", TMA_MSG);
        write(STDOUT_FILENO, msg, strlen(msg));
        continue;
      }
      break;
    } else if (strcmp(args[0], "pwd") == 0) {
      if (args[1]) {
        const char *msg = FORMAT_MSG("pwd", TMA_MSG);
        write(STDOUT_FILENO, msg, strlen(msg));
        continue;
      }
      getcwd(cwd_buffer, sizeof(cwd_buffer));
      write(STDOUT_FILENO, cwd_buffer, strlen(cwd_buffer));
      write(STDOUT_FILENO, "\n", 1);
      continue;
    } else if (strcmp(args[0], "cd") == 0) {
      if (args[2]) {
        const char *msg = FORMAT_MSG("cd", TMA_MSG);
        write(STDOUT_FILENO, msg, strlen(msg));
        continue;
      }
      char *path = args[1];
      if (!path)
        path = getenv("HOME");
      else if (path[0] == '~') {
        static char expanded[4096];
        snprintf(expanded, sizeof(expanded), "%s%s", getenv("HOME"), path + 1);
        path = expanded;
      } else if (path[0] == '-') {
        path = lastPath;
      }

      if (chdir(path) == -1) {
        const char *msg = FORMAT_MSG("cd", CHDIR_ERROR_MSG);
        write(STDOUT_FILENO, msg, strlen(msg));
        continue;
      }
      strncpy(lastPath, cwd_buffer, sizeof(lastPath));
      continue;
    } else if (strcmp(args[0], "history") == 0) {
      if (args[1]) {
        const char *msg = FORMAT_MSG("history", TMA_MSG);
        write(STDOUT_FILENO, msg, strlen(msg));
        continue;
      }
      print_history();
      continue;
    } else if (strcmp(args[0], "help") == 0) {
      const char *msgs[] = {
          FORMAT_MSG("help", HELP_HELP_MSG), FORMAT_MSG("cd", CD_HELP_MSG),
          FORMAT_MSG("exit", EXIT_HELP_MSG), FORMAT_MSG("pwd", PWD_HELP_MSG),
          FORMAT_MSG("history", HISTORY_HELP_MSG)};
      for (int i = 0; i < 5; i++)
        write(STDOUT_FILENO, msgs[i], strlen(msgs[i]));
      continue;
    }

    // -------------------- External Commands --------------------
    pid_t pid = fork();
    if (pid < 0) {
      const char *err = FORMAT_MSG("shell", FORK_ERROR_MSG);
      write(STDOUT_FILENO, err, strlen(err));
      continue;
    } else if (pid == 0) {
      signal(SIGINT, SIG_DFL); // restore default Ctrl+C
      execvp(args[0], args);
      const char *err = "exec failed\n";
      write(STDOUT_FILENO, err, strlen(err));
      _exit(1);
    } else {
      if (!bg) {
        waitpid(pid, &status, 0);
      } else {
        char buf[64];
        int len = snprintf(buf, sizeof(buf), "[Background PID %d]\n", pid);
        write(STDOUT_FILENO, buf, len);
      }
    }
  }

  const char *exit_msg = "\nExiting shell...\n";
  write(STDOUT_FILENO, exit_msg, strlen(exit_msg));

  // Free history
  for (int i = 0; i < hist_size; i++)
    free(input_hist[i]);
  return 0;
}
