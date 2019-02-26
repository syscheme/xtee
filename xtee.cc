extern "C"
{
#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
}

#include <iostream>
#include <string>
#include <vector>
#include <set>

#define EOL "\r\n"
#define CHECK_INTERVAL (200) //  200msec

bool gQuit = false;

void usage()
{
  std::cout << "Usage: xtee {-n|[-a] <file>} [-s <bps>] [-k <bytes>] [-t <secs>] [-d <secs>] [-q <secs>]" EOL
            << "            [-c <cmdline>] [-l <TARGET>:<SOURCE>]" EOL EOL
            << "Options:" EOL
            << "  -v <level>           verbose level, default 4 to output progress onto stderr" EOL
            << "  -a                   append to the output file" EOL
            << "  -n                   no output file other than stdout" EOL
            << "  -s <bps>             limits the transfer bitrate at reading from stdin in bps" EOL
            << "  -k <bytes>           skips a certain amount of bytes at the beginning of reading from stdin" EOL
            << "  -t <secs>            skips the given seconds of data at reading from stdin" EOL
            << "  -d <secs>            duration in seconds to run" EOL
            << "  -q <secs>            timeout in seconds when no more data can be read from stdin" EOL
            << "  -c <cmdline>         the child command line to execute" EOL
            << "  -l <TARGET>:<SOURCE> links the source fd to the target fd, <TARGET> or <SOURCE> is in format of" EOL
            << "                       \"[<cmdNo>.]<fd>\", where <cmdNo> is the sequence number of -c options, and" EOL
            << "                       default '0.' refers to the xtee command itself" EOL
            << "  -h                   display this screen" EOL EOL
            << "Examples:" EOL
            << "  a) the following command results the same as runing \"ls -l | sort and ls -l | grep txt\"，but the" EOL
            << "     outputs of the single round of \"ls -l\" will be taken by both \"sort\" and \"grep\" commands:" EOL
            << "       xtee -c \"ls -l\" -c \"sort\" -c \"grep txt\" -l 1.1:2.0 -l 1.1:3.0" EOL
            << "  b) the following equal commands download from a web at a limited speed of 3.75Mbps, zip and save" EOL
            << "     as a file:" EOL
            << "       xtee -c \"wget -O - http://…\" -c \"zip - -o file.zip\" -l 0:1.1 -l 2.0:1 -n -s 3750000" EOL
            << "       wget -O - http://… | xtee -c \"zip - -o file.zip\" -l 2.0:1 -n -s 3750000" EOL
            << "       wget -O - http://… | xtee -n -s 3750000 | zip - -o file.zip" EOL
            << EOL;
}

void logerr(const char fmt, ...)
{
}

#define trace

typedef struct
{
  bool noOutFile;
  bool append;
  long bitrate;
  long bytesToSkip;
  int secsToSkip;
  int secsDuration;
  int secsTimeout;
  unsigned int logflags;
} CmdOptions;

CmdOptions cmdOpts = {
    .noOutFile = false,
    .append = false,
    .bitrate = -1,
    .bytesToSkip = -1,
    .secsToSkip = -1,
    .secsDuration = -1,
    .secsTimeout = -1,
    .logflags = 0};

typedef int Pipe[2];
typedef Pipe StdioPipes[3];

#define PSTDIN(_PIO) (_PIO[0])
#define PSTDOUT(_PIO) (_PIO[1])
#define PSTDERR(_PIO) (_PIO[2])

typedef std::vector<char *> Strings;
Strings childCommands, fdLinks;
typedef std::set<int> FDSet;

typedef struct _ChildStub
{
  char *cmd;
  int stdio[3];
  FDSet out2fds, err2fds;
  int pid;
  int status;
} ChildStub;

typedef std::vector<ChildStub> Children;
Children children;
FDSet out2fds;

#define CHILDIN(_CH) PSTDIN(_CH.stdio)
#define CHILDOUT(_CH) PSTDOUT(_CH.stdio)
#define CHILDERR(_CH) PSTDERR(_CH.stdio)

#define IS_VALID_FLAG_SET(_FD, _SET) (_FD >= 0 && FD_ISSET(_FD, &_SET))
#define SET_VALID_FD_IN(_FD, _FDSET, _MAXFD) \
  if (_FD >= 0)                              \
  {                                          \
    FD_SET(_FD, &_FDSET);                    \
    if (_MAXFD < _FD)                        \
      _MAXFD = _FD;                          \
  }

char buf[1024] = {0};
ssize_t procfd(int &fd, fd_set &fdread, fd_set &fderr, const FDSet &fwdset, int defaultfd)
{
  ssize_t n = 0;

  if (IS_VALID_FLAG_SET(fd, fdread) && (n = ::read(fd, buf, sizeof(buf))) > 0)
  {
    if (fwdset.empty())
      ::write(defaultfd, buf, n);
    else
    {
      for (FDSet::const_iterator it = fwdset.begin(); it != fwdset.end(); it++)
      {
        if (*it > 0)
          ::write(*it, buf, n);
      }
    }
  }

  if (fd > STDERR_FILENO && IS_VALID_FLAG_SET(fd, fderr))
  {
    ::close(fd);
    fd = -1;
    n =-1;
  }

  return n;
}

void closePipesToChild(ChildStub& child)
{
    for (int j = 0; j < 3; j++)
    {
      int &fd = child.stdio[j];
      if (fd > STDERR_FILENO)
        ::close(fd);
      fd = -1;
    }
}

int main(int argc, char *argv[])
{
  int ret=0;
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

    default:
      ret = -1;
    case 'h':
    case '?':
      usage();
      return ret;
    }
  }

  for (size_t i = 0; i < childCommands.size(); i++)
  {
    char *childcmd = childCommands[i]; // (char*) childCommands[i].c_str();

    StdioPipes stdioPipes;
    ::pipe(PSTDIN(stdioPipes));
    ::pipe(PSTDOUT(stdioPipes));
    ::pipe(PSTDERR(stdioPipes));

    pid_t pidChild = fork(); // create child process that is a clone of the parent
    if (pidChild == 0)
    {
      printf("%s wrapping child-%u[%s]...\n", argv[0], i, childcmd);

      // this the child process, remap the pipe to local stdXX
      ::dup2(PSTDIN(stdioPipes)[0], STDIN_FILENO);
      ::close(PSTDIN(stdioPipes)[0]), ::close(PSTDIN(stdioPipes)[1]);
      ::dup2(PSTDOUT(stdioPipes)[1], STDOUT_FILENO);
      ::close(PSTDOUT(stdioPipes)[0]), ::close(PSTDOUT(stdioPipes)[1]);
      ::dup2(PSTDERR(stdioPipes)[0], STDERR_FILENO);
      ::close(PSTDERR(stdioPipes)[0]), ::close(PSTDERR(stdioPipes)[1]);

      int childargc = 0;
      char *childargv[32];
      for (char *p2 = strtok(childcmd, " "); p2 && childargc < (int)(sizeof(childargv) / sizeof(childargv[0])) - 1; childargc++)
      {
        childargv[childargc] = p2;
        p2 = strtok(0, " ");
      }

      childargv[childargc++] = NULL;

      int ret = execvp(childargv[0], childargv);

      // for (int j=0; j < 10000; j++)
      //   printf("%s child-%u[%s]: %d\n", argv[0], i, childcmd, j);

      printf("%s child-%u[%s] exits(%d)\n", argv[0], i, childcmd, ret);
      return ret; // end of the child process
    }

    // this is the parent process, close the pipe[0]s and only leave pipe[1]s open
    ChildStub child;
    child.cmd = childcmd;

    // file descriptor unused in parent
    child.stdio[0] = child.stdio[1] = child.stdio[2] =-1;
    ::close(PSTDIN(stdioPipes)[0]);  child.stdio[0] = PSTDIN(stdioPipes)[1]; 
    ::close(PSTDOUT(stdioPipes)[1]); child.stdio[1] = PSTDOUT(stdioPipes)[0];
    ::close(PSTDERR(stdioPipes)[1]); child.stdio[2] = PSTDERR(stdioPipes)[0];
    // for (int j = 0; j < 3; j++)
    // {
    //   child.stdio[j] = stdioPipes[j][1];
    //   // ::fcntl(child.stdio[j], F_SETFL, O_NONBLOCK);
    // }

    child.pid = pidChild;
    child.status = 0;

    children.push_back(child);
  }

  printf("%s started %u child(s), making up the links\n", argv[0], children.size());
  for (size_t i = 0; i < fdLinks.size(); i++)
  {
    const char *delimitor = strchr(fdLinks[i], ':'); // strchr(fdLinks[i].c_str(), ':');
    if (NULL == delimitor)
      continue;
    // TODO
  }

  // scan and audit the linkages
  for (size_t i = 0; i < children.size(); i++)
  {
    ChildStub &child = children[i];
    if (child.out2fds.size() == 1)
    {
      // directly connect the link
      int dest = (*child.out2fds.begin());
      if (dest > STDERR_FILENO)
      {
        ::dup2(CHILDOUT(child), dest);
        ::close(CHILDOUT(child));
        CHILDOUT(child) = -1;
      }
    }

    if (child.err2fds.size() == 1) 
    {
      // directly connect the link
      int dest = (*child.err2fds.begin());
      if (dest > STDERR_FILENO)
      {
        ::dup2(CHILDERR(child), dest);
        ::close(CHILDERR(child));
        CHILDERR(child) = -1;
      }
    }
  }

  int maxTimeouts = cmdOpts.secsTimeout * 1000 / CHECK_INTERVAL;

  for (int timeouts = 0; !gQuit && (maxTimeouts < 0 || timeouts < maxTimeouts);)
  {
    fd_set fdread, fderr;
    FD_ZERO(&fdread);
    FD_ZERO(&fderr);

    int maxfd = -1;
    SET_VALID_FD_IN(STDIN_FILENO, fdread, maxfd); // the STDIN of the parent process

    for (size_t i = 0; i < children.size(); i++)
    {
      ChildStub &child = children[i];
      int fd = CHILDOUT(child);
      SET_VALID_FD_IN(fd, fdread, maxfd);
      SET_VALID_FD_IN(fd, fderr, maxfd);

      fd = CHILDERR(child);
      SET_VALID_FD_IN(fd, fdread, maxfd);
      SET_VALID_FD_IN(fd, fderr, maxfd);
    }

    if (maxfd <= STDERR_FILENO)
    {
      // no child seems alive, quit
      gQuit = true;
      break;
    }

    struct timeval timeout;
    timeout.tv_sec = CHECK_INTERVAL / 1000;
    timeout.tv_usec = (CHECK_INTERVAL % 1000) * 1000;

    int rc = select(maxfd + 1, &fdread, NULL, &fderr, &timeout);
    //			printf("After select [%d %d %d] %s\n", fdread.fd_count, fdwrite.fd_count, fderr.fd_count, TimeUtil::TimeToUTC(now(), buf, sizeof(buf)-2, true));
    if (gQuit)
      break;

    if (rc < 0) // select(), quit
    {

      break;
    }
    else if (0 == rc) // timeout
    {
      timeouts++;

      continue;
    }

    // about this stdin
    {
      int fdStdin = STDIN_FILENO;
      ssize_t n = procfd(fdStdin, fdread, fderr, out2fds, STDOUT_FILENO);
      // TODO: bitrate control
    }

    for (size_t i = 0; !gQuit && i < children.size(); i++)
    {
      ChildStub &child = children[i];

      // about the child's stdout
      procfd(CHILDOUT(child), fdread, fderr, child.out2fds, STDOUT_FILENO);

      // about the child's stderr
      procfd(CHILDERR(child), fdread, fderr, child.err2fds, STDERR_FILENO);
    }

//    for (size_t j = 0; !gQuit && j < children.size(); j++)
//    {
//      ChildStub &child = children[j];
//      pid_t wpid = waitpid(child.pid, &child.status, WNOHANG | WUNTRACED); // instantly return
//      if (wpid != child.pid || WIFEXITED(child.status))                    // return wpid == child.pid && WIFEXITED(child.status) ? WEXITSTATUS(child.status) : -1;
//        closePipesToChild(child);
//    }

  } // end of select() loop

  // close all pipes that are still openning
  for (size_t i = 0; i < children.size(); i++)
  {
    for (int j = 0; j < 3; j++)
    {
      int &fd = children[i].stdio[j];
      if (fd > STDERR_FILENO)
        ::close(fd);
      fd = -1;
    }
  }

  return 0;
}
