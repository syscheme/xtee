#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <iostream>
#include <string>
#include <vector>

#define EOL "\r\n"

void usage()
{
  std::cout << "Usage: xtee {-n|[-a] <file>} [-s <bps>] [-k <bytes>] [-t <secs>] [-d <secs>] [-q <secs>]" EOL
            << "       xtee [-c <cmdline>] [-l <TARGET>:<SOURCE>]" EOL
            << "Options:" EOL
            << "  -v <level>           verbose level, default 4 to output progress onto PSTDERR(stdioPipes)" EOL
            << "  -a                   append to the output file" EOL
            << "  -n                   no output file in addition to PSTDOUT(stdioPipes)" EOL
            << "  -s <bps>             limit the transfer bitrate at beginning of reading from PSTDIN(stdioPipes) in bps" EOL
            << "  -k <bytes>           skip a certain number of bytes at reading from PSTDIN(stdioPipes)" EOL
            << "  -t <secs>            skip the given seconds of data read from PSTDIN(stdioPipes)" EOL
            << "  -d <secs>            duration in seconds to run" EOL
            << "  -q <secs>            timeout in seconds when no more data can be read from PSTDIN(stdioPipes)" EOL
            << "  -c <cmdline>         the child command line to execute" EOL
            << "  -l <TARGET>:<SOURCE> link the source fd to the target fd, <TARGET> or <SOURCE> is in format of" EOL
            << "                        \"[<cmdId>.]<fd>\", where <cmdId> is the sequence number of -c options, and" EOL
            << "                        default '0.' refers to the xtee command itself" EOL
            << "  -h                   display this screen" EOL
            << "Examples:" EOL
            << "  a) the following command results same as runing ls -l | sort and ls -l | grep txt，but the outputs" EOL
            << "      of only one round of ls -l is taken by both sort and grep" EOL
            << "       xtee -c \"ls -l\" -c \"sort\" -c \"grep txt\" -l 1.1:2.0 -l 1.1:3.0" EOL
            << "  b) the following command downloads from a web at a limited speed of 3.75Mbps, zip and save as a file" EOL
            << "       xtee -c \"wget - http://…\" -c \"zip - -o file.zip\" -l 0:1.1 -l 2.0:1 -n -s 3750000" EOL
            << EOL;
}


void logerr(const char fmt, ...)
{

}

#define trace

typedef struct {
  bool noOutFile;
  bool append;
  long bitrate;
  long bytesToSkip;
  int  secsToSkip;
  int  secsDuration;
  int  secsTimeout;
  unsigned int logflags;
} CmdOptions;

CmdOptions cmdOpts = {
  .noOutFile = false,
  .append = false,
  .bitrate = -1,
  .bytesToSkip =-1,
  .secsToSkip =-1,
  .secsDuration =-1,
  .secsTimeout =-1,
  .logflags =0
};

typedef int Pipe[2];
typedef Pipe StdioPipes[3];

#define PSTDIN(_PIO)  (_PIO[0])
#define PSTDOUT(_PIO) (_PIO[1])
#define PSTDERR(_PIO) (_PIO[2])

typedef std::vector < std::string > Strings;
Strings childCommands, fdLinks;

typedef struct _ChildStub
{
  int stdio[3];
  int pid;
  int ret;
} ChildStub;

typedef std::vector < ChildStub > Children;
Children children;

int main(int argc, char *argv[])
{
  if (argc < 2)
  {
    usage();
    return -1;
  }

  int opt = 0;
  while (-1 != (opt = getopt(argc, argv, "hnas:k:t:d:q:c:l:")))
  {
    switch (opt)
    {
    case 'n':
      cmdOpts.noOutFile = true;
      break;

    case 'a':
      cmdOpts.append = true;
      break;

    case 's':
      cmdOpts.bitrate = atol(optarg);
      break;

    case 'k':
      cmdOpts.bytesToSkip = atol(optarg);
      break;

    case 't':
      cmdOpts.secsToSkip = atoi(optarg);
      break;

    case 'd':
      cmdOpts.secsDuration = atoi(optarg);
      break;

    case 'q':
      cmdOpts.secsTimeout = atoi(optarg);
      break;

    case 'c':
      childCommands.push_back(optarg);
      break;

    case 'l':
      fdLinks.push_back(optarg);
      break;

    case 'h':
    case '?':
    default:
      usage();
      return 0;
    }
  }

  int maxfd =-1;

  for (size_t i = 0; i < childCommands.size(); i++)
  {
    char* childcmd = (char*) childCommands[i].c_str();

    StdioPipes stdioPipes;
    ::pipe(PSTDIN(stdioPipes));
    ::pipe(PSTDOUT(stdioPipes));
    ::pipe(PSTDERR(stdioPipes));

    pid_t pidChild = fork(); // create child process that is a clone of the parent
    if (pidChild == 0)
    {
      // this the child process, remap the pipe to local stdXX
      ::dup2(PSTDIN(stdioPipes)[0], STDIN_FILENO);
      ::close(PSTDIN(stdioPipes)[0]), ::close(PSTDIN(stdioPipes)[1]);
      ::dup2(PSTDOUT(stdioPipes)[0], STDOUT_FILENO);
      ::close(PSTDOUT(stdioPipes)[0]), ::close(PSTDOUT(stdioPipes)[1]);
      ::dup2(PSTDERR(stdioPipes)[0], STDOUT_FILENO);
      ::close(PSTDERR(stdioPipes)[0]), ::close(PSTDERR(stdioPipes)[1]);

      printf("%s wrapping child-%u[%s]...\n", argv[0], i, childcmd);

      int childargc = 0;
      char *childargv[32];
      for (char *p2 = strtok(childcmd, " "); p2 && childargc < (sizeof(childargv) / sizeof(childargv[0])) - 1; childargc++)
      {
        childargv[childargc] = p2;
        p2 = strtok(0, " ");
      }

      childargv[childargc++] = NULL;
      
      int ret = execvp(childargv[0], childargv);
      printf("%s child-%u[%s] finished, ret(%d)\n", argv[0], i, childcmd, ret);
      return ret; // end of the child process
    }

    // this is the parent process, close the pipe[0]s and only leave pipe[1]s open
    ::close(PSTDIN(stdioPipes)[0]);  // file descriptor unused in parent
    ::close(PSTDOUT(stdioPipes)[0]); // file descriptor unused in parent
    ::close(PSTDERR(stdioPipes)[0]); // file descriptor unused in parent

    ChildStub child = {
      .stdio = {PSTDIN(stdioPipes)[1], PSTDOUT(stdioPipes)[1], PSTDERR(stdioPipes)[1] },
      .pid = pidChild,
      .ret =0,
    };

    if (maxfd < PSTDIN(child.stdio))
      maxfd = PSTDIN(child.stdio);
    if (maxfd < PSTDOUT(child.stdio))
      maxfd = PSTDOUT(child.stdio);
    if (maxfd < PSTDERR(child.stdio))
      maxfd = PSTDERR(child.stdio);

    children.push_back(child);
  }

  printf("%s started %d child(s), making up the links\n", argv[0], children.size());
  for (size_t i =0; i < fdLinks.size(); i++)
  {
    const char* delimitor = strchr(fdLinks[i].c_str(), ':');
    if (NULL == delimitor)
      continue;
    // TODO
  }

  while (true)
  {
  //   FD_SET fdread, fdwrite, fderr;
  //   FD_ZERO(&fdread), FD_ZERO(&fdwrite), FD_ZERO(&fderr);
  //   for (size_t i =0; i < children.size(); i++)
  //   {
  //     if (PSTDIN(children[i].stdio) >0)
  //         FD_SET(&fdwrite, PSTDIN(children[i].stdio));

  //     if (PSTDOUT(children[i].stdio) >0)
  //         FD_SET(&fdread, PSTDOUT(children[i].stdio));

  //     if (PSTDERR(children[i].stdio) >0)
  //     {
  //         FD_SET(&fdread, PSTDERR(children[i].stdio))
  //         FD_SET(&fderr, PSTDERR(children[i].stdio))
  //     }
  //   }

    // rc = ::select(fdread, fdwrite, fderr, maxfd);


  }

    // int status;
    // pid_t wpid = waitpid(pid, &status, 0); // wait for child to finish before exiting
    // return wpid == pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
