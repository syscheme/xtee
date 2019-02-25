#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void usage()
{
  
}

void logerr(const char fmt, ...)
{
}

static unsigned int logflags =0;

int main(int argc, char *argv[])
{
  if (argc <2)
	return -1;

  int fds[3][2];                      // an array that will hold two file descriptors
  for (int i=0; i<3; i++)
  	pipe(fds[i]);                 // populates fds with two file descriptors

  pid_t pid = fork();              // create child process that is a clone of the parent
  
  if (pid == 0) {                  // if pid == 0, then this is the child process
    dup2(fds[0][0], STDIN_FILENO);    // fds[0] (the read end of pipe) donates its data to file descriptor 0
    close(fds[0][0]);                 // file descriptor no longer needed in child since stdin is a copy
    close(fds[0][1]);                 // file descriptor unused in child

    dup2(fds[1][0], STDOUT_FILENO);    // fds[0] (the read end of pipe) donates its data to file descriptor 0
    close(fds[1][0]);                 // file descriptor no longer needed in child since stdin is a copy
    close(fds[1][1]);                 // file descriptor unused in child

    dup2(fds[2][0], STDERR_FILENO);    // fds[0] (the read end of pipe) donates its data to file descriptor 0
    close(fds[2][0]);                 // file descriptor no longer needed in child since stdin is a copy
    close(fds[2][1]);                 // file descriptor unused in child

    printf("%s wrapping child[%s]...\n", argv[0], argv[1]);

    int childargc = 0;
    char *childargv[32];

    for (char *p2 = strtok(argv[1], " "); p2 && argc < (sizeof(childargv)/sizeof(childargv[0]))-1; childargc++)
    {
      childargv[childargc] = p2;
      p2 = strtok(0, " ");
    }
    childargv[childargc++] = NULL;
    
    if (execvp(childargv[0], argv) < 0) exit(0);  // run sort command (exit if something went wrong)
  } 

  // if we reach here, we are in parent process
  close(fds[0][0]);                 // file descriptor unused in parent
  close(fds[1][0]);                 // file descriptor unused in parent
  close(fds[2][0]);                 // file descriptor unused in parent
  
  const char *words[] = {"pear", "peach", "apple"};
  // write input to the writable file descriptor so it can be read in from child:
  size_t numwords = sizeof(words)/sizeof(words[0]);
  for (size_t i = 0; i < numwords; i++) {
    dprintf(fds[0][1], "%s\n", words[i]); 
  }

  close(fds[0][1]);
  close(fds[1][1]);
  close(fds[2][1]);

  int status;
  pid_t wpid = waitpid(pid, &status, 0); // wait for child to finish before exiting
  return wpid == pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
