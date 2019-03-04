#ifndef __XTEE_HH__
#define __XTEE_HH__

#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <map>

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

#define EOL "\r\n"
#define CHECK_INTERVAL (500) // 500msec

#define LOGF_TRACE (1 << 0)
#define LOGF_ERROR (1 << 1)

// -----------------------------
// class Xtee
// -----------------------------
class Xtee
{
public:
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
  } Options;

  Xtee();
  virtual ~Xtee() {}

  int run();

  int pushCommand(char* cmd);
  int pushLink(char* link);

  Options _options;

protected:
  typedef int Pipe[2];
  typedef Pipe StdioPipes[3];
  typedef std::set<int> FDSet;

  typedef struct _ChildStub
  {
    int  idx;
    char *cmd;
    int stdio[3];
    FDSet fwdStdout, fwdStderr;
    int pid;
    int status;
  } ChildStub;

  typedef std::vector<ChildStub> Children;
  Children _children;
  FDSet _stdin2fwd;
  typedef std::map<int, FDSet> ForwardMap;

  int     log(unsigned int category, const char *fmt, ...);
  ssize_t procfd(int &fd, fd_set &fdread, fd_set &fderr, const FDSet &fwdset, int defaultfd, int childIdx = -1);
  int     closePipesToChild(ChildStub &child);

  bool _bQuit = false;
  typedef std::vector<char *> Strings;
  Strings _childCommands, _fdLinks;
};

#endif // __XTEE_HH__

