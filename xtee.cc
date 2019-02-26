extern "C"
{
#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/time.h>
}

#include <iostream>
#include <string>
#include <vector>
#include <set>

#define EOL "\r\n"
#define CHECK_INTERVAL (500) // 500msec

bool gQuit = false;

// -----------------------------
// usage()
// -----------------------------
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

#define LOGF_TRACE (1 << 0)
#define LOGF_ERROR (1 << 1)

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
    .logflags = 0xff};

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
  int idx;
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

// -----------------------------
// log()
// -----------------------------
#define LOG_LINE_MAX_BUF (256)
int log(unsigned int category, const char *fmt, ...)
{
  if (0 == (cmdOpts.logflags & category))
    return 0;

  char msg[LOG_LINE_MAX_BUF];
  va_list args;

  va_start(args, fmt);
  int nCount = ::vsnprintf(msg, LOG_LINE_MAX_BUF - 8, fmt, args);
  va_end(args);
  msg[LOG_LINE_MAX_BUF - 8] = '\0';

  if (nCount < 0)
    nCount = 0;

  msg[nCount++] = '\r';
  msg[nCount++] = '\n';
  msg[nCount++] = '\0';
  return ::write(STDERR_FILENO, msg, nCount);
}

static long long now()
{
  struct timeval tmval;
  if (0 != gettimeofday(&tmval, (struct timezone *)NULL))
    return 0;

  return (tmval.tv_sec * 1000LL + tmval.tv_usec / 1000);
}

// -----------------------------
// procfd()
// -----------------------------
char buf[1024] = {0};
ssize_t procfd(int &fd, fd_set &fdread, fd_set &fderr, const FDSet &fwdset, int defaultfd, int childIdx = -1)
{
  ssize_t n = 0;

  if (IS_VALID_FLAG_SET(fd, fdread) && (n = ::read(fd, buf, sizeof(buf))) > 0)
  {
    if (fwdset.empty())
    {
      if (defaultfd == STDERR_FILENO && childIdx > 0)
      {
        char cIdent[20];
        snprintf(cIdent, sizeof(cIdent) - 2, "CH%02u> ", (unsigned)childIdx);
        ::write(defaultfd, cIdent, strlen(cIdent));
      }

      ::write(defaultfd, buf, n);

      if (defaultfd == STDERR_FILENO && childIdx > 0)
        ::write(defaultfd, EOL, sizeof(EOL) - 1);
    }
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
    log(LOGF_TRACE, "CH%02u[%s] closing error fd(%d)", childIdx, fd);
    ::close(fd);
    fd = -1;
    n = -1;
  }

  return n;
}

// -----------------------------
// closePipesToChild()
// -----------------------------
int closePipesToChild(ChildStub &child)
{
  int nClosed = 0;
  for (int j = 0; j < 3; j++)
  {
    int &fd = child.stdio[j];
    if (fd > STDERR_FILENO)
    {
      ::fsync(fd);
      ::close(fd);
      fd = -1;
      nClosed++;
    }
  }

  if (nClosed > 0)
    log(LOGF_TRACE, "closed %d link(s) to C%02d[%s]", nClosed, child.idx, child.cmd);

  return nClosed;
}

// -----------------------------
// main()
// -----------------------------
int main(int argc, char *argv[])
{
  int ret = 0;
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
    char *childcmd = childCommands[i];

    // pa step 1. init pipe pairs
    StdioPipes stdioPipes;
    memset(&stdioPipes, -1, sizeof(stdioPipes));
    ::pipe(PSTDIN(stdioPipes));
    ::pipe(PSTDOUT(stdioPipes));
    ::pipe(PSTDERR(stdioPipes));

    // pa step 2. create child process that is a clone of the parent
    pid_t pidChild = fork();
    if (pidChild == 0)
    {
      log(LOGF_TRACE, "CH%02u[%s] spawned: %d<%d, %d>%d, %d>%d", i + 1, childcmd,
          PSTDIN(stdioPipes)[0], PSTDIN(stdioPipes)[1], PSTDOUT(stdioPipes)[0], PSTDOUT(stdioPipes)[1], PSTDERR(stdioPipes)[0], PSTDERR(stdioPipes)[1]);

      std::string cmdline = childcmd;

      // this the child process
      // child step 1. remap the pipe to local stdXX
      ::dup2(PSTDIN(stdioPipes)[0], STDIN_FILENO);
      ::close(PSTDIN(stdioPipes)[0]), ::close(PSTDIN(stdioPipes)[1]);
      ::dup2(PSTDOUT(stdioPipes)[1], STDOUT_FILENO);
      ::close(PSTDOUT(stdioPipes)[0]), ::close(PSTDOUT(stdioPipes)[1]);
      ::dup2(PSTDERR(stdioPipes)[1], STDERR_FILENO);
      ::close(PSTDERR(stdioPipes)[0]), ::close(PSTDERR(stdioPipes)[1]);

      // child step 2. prepare the child command line
      int childargc = 0;
      char *childargv[32];
      for (char *p2 = strtok(childcmd, " "); p2 && childargc < (int)(sizeof(childargv) / sizeof(childargv[0])) - 1; childargc++)
      {
        childargv[childargc] = p2;
        p2 = strtok(0, " ");
      }

      childargv[childargc++] = NULL;

      // child step 3. launch the child command line
      log(LOGF_TRACE, "CH%02u[%s] starts", i + 1, cmdline.c_str());
      int ret = execvp(childargv[0], childargv);

      // child step 4. exitting
      log(LOGF_TRACE, "CH%02u[%s] exited (%d)", i + 1, cmdline.c_str(), ret);
      fsync(STDOUT_FILENO);
      fsync(STDERR_FILENO);

      exit(ret); // end of the child process
    }

    // this is the parent process
    if (pidChild <= 0)
    {
      log(LOGF_ERROR, "failed to create CH%02u[%s]: pid(%d)", i, childcmd, pidChild);
      return -100;
    }

    // pa step 3. close the pipe peers and save a stub to the child
    ChildStub child;
    child.idx = i + 1;
    child.cmd = childcmd;
    child.pid = pidChild;
    child.status = 0;

    // file descriptor unused in parent
    child.stdio[0] = child.stdio[1] = child.stdio[2] = -1;
    ::close(PSTDIN(stdioPipes)[0]);
    child.stdio[0] = PSTDIN(stdioPipes)[1];
    ::close(PSTDOUT(stdioPipes)[1]);
    child.stdio[1] = PSTDOUT(stdioPipes)[0];
    ::close(PSTDERR(stdioPipes)[1]);
    child.stdio[2] = PSTDERR(stdioPipes)[0];
    for (int j = 0; j < 3; j++)
      ::fcntl(child.stdio[j], F_SETFL, O_NONBLOCK);

    children.push_back(child);
    log(LOGF_TRACE, "CH%02u[%s] created: pid(%d) %d>%d, %d<%d, %d<%d", child.idx, child.cmd, child.pid,
        PSTDIN(stdioPipes)[0], PSTDIN(stdioPipes)[1], PSTDOUT(stdioPipes)[0], PSTDOUT(stdioPipes)[1], PSTDERR(stdioPipes)[0], PSTDERR(stdioPipes)[1]);
  }

  // pa step 4. build up the link exchanges
  log(LOGF_TRACE, "created %u child(s), making up the links", children.size());
  for (size_t i = 0; i < fdLinks.size(); i++)
  {
    const char *delimitor = strchr(fdLinks[i], ':'); // strchr(fdLinks[i].c_str(), ':');
    if (NULL == delimitor)
      continue;
    // TODO
  }

  // scan and compress the linkages
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

  // pa step 5. start the main loop
  int maxTimeouts = cmdOpts.secsTimeout * 1000 / CHECK_INTERVAL;
  bool bChildCheckNeeded = false;
  int nIdles = 0;

  for (int timeouts = 0; !gQuit && (maxTimeouts < 0 || timeouts < maxTimeouts);)
  {
    // pa step 5.1 check the child processes
    if (bChildCheckNeeded || nIdles > (10000 / CHECK_INTERVAL))
    {
      nIdles = 0;
      for (size_t j = 0; !gQuit && j < children.size(); j++)
      {
        ChildStub &child = children[j];
        pid_t wpid = waitpid(child.pid, &child.status, WNOHANG | WUNTRACED); // instantly return
        if (wpid != 0)                                                       // WNOHANG returns 0 when child is still running
        {
          int c = closePipesToChild(child);
          if (wpid == child.pid)
            log(LOGF_TRACE, "CH%02u[%s] exited: pid(%d) status(0x%x)", child.idx, child.cmd, child.pid, child.status);
          else if (c > 0)
            log(LOGF_TRACE, "CH%02u[%s] gone: pid(%d)", child.idx, child.cmd, child.pid);
        }
      }
    }

    // pa step 5.2 prepare fdset for select()
    int bytesChildrenIO = 0;

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

    if (maxfd <= STDERR_FILENO && children.size() > 0)
    {
      // no child seems alive, quit
      gQuit = true;
      break;
    }

    // pa step 5.3 do select()
    struct timeval timeout;
    timeout.tv_sec = CHECK_INTERVAL / 1000;
    timeout.tv_usec = (CHECK_INTERVAL % 1000) * 1000;

    int rc = select(maxfd + 1, &fdread, NULL, &fderr, &timeout);
    if (gQuit)
      break;

    // pa step 5.4  select() dispatching
    if (rc < 0) // select() err, quit
    {
      log(LOGF_ERROR, "io wait got (%d) error(%d), quiting", rc, errno);
      break;
    }
    else if (0 == rc) // timeout
    {
      timeouts++;
      bChildCheckNeeded = true;
      continue;
    }

    // pa step 5.5 about this stdin
    {
      int fdStdin = STDIN_FILENO;
      ssize_t n = procfd(fdStdin, fdread, fderr, out2fds, STDOUT_FILENO);

      if (n <= 0 && children.empty())
        break;

      if (n > 0)
      {
        // TODO: bitrate control
      }
    }

    // pa step 5.6 scan if any child has IO occured
    for (size_t i = 0; !gQuit && i < children.size(); i++)
    {
      ChildStub &child = children[i];
      ssize_t n = 0;

      // about the child's stdout
      n = procfd(CHILDOUT(child), fdread, fderr, child.out2fds, STDOUT_FILENO, child.idx);

      if (n < 0)
        bChildCheckNeeded = true;
      else
        bytesChildrenIO += n;

      // about the child's stderr
      n = procfd(CHILDERR(child), fdread, fderr, child.err2fds, STDERR_FILENO, child.idx);
      if (n < 0)
        bChildCheckNeeded = true;
      else
        bytesChildrenIO += n;
    }

    if (bytesChildrenIO <= 0)
      nIdles++;

  } // end of select() loop

  // pa step 6. close all pipes that are still openning
  log(LOGF_TRACE, "end of loop, cleaning up %u child(s)", children.size());
  for (size_t i = 0; i < children.size(); i++)
    closePipesToChild(children[i]);

  return 0;
}
