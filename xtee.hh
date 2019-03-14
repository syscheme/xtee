#ifndef __XTEE_HH__
#define __XTEE_HH__

#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <map>

extern "C"
{
#include <stdio.h>
#include <stdlib.h>
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
    long kbps;
    long bytesToSkip;
    int  secsToSkip;
    int  secsDuration;
    int  secsTimeout;
    unsigned int logflags;
  } Options;

  Xtee();
  virtual ~Xtee() {}

  bool init();
  int run();

  int pushCommand(char* cmd);
  int pushLink(char* link);

  void printLinks();

  Options _options;

protected:
  typedef int Pipe[2];
  typedef Pipe StdioPipes[3];
  typedef std::set<int> FDSet;
  typedef std::map<int, FDSet> FDIndex;

  typedef struct _ChildStub
  {
    int  idx;
    char *cmd;
    int stdio[3];
    // FDSet fwdStdout, fwdStderr;
    int pid;
    int status;
  } ChildStub;

  typedef std::vector<ChildStub> Children;
  Children _children;
  FDSet _stdin2fwd;

  FDIndex _fd2fwd;
  FDIndex _fd2src;

  bool    link(int fdIn, int fdTo);
  void    unlink(int fdIn, int fdTo);
  std::string closeSrcFd(int& fdSrc);
  std::string closeDestFd(int& fdDest);
  int     errlog(unsigned int category, const char *fmt, ...);
  
  //@return bytes read from the fd, -1 if error occured at reading
  int     checkAndForward(int &fd, int defaultfd, int childIdx = -1);
  void    closePipesToChild(ChildStub &child);
  int     stdinQoS();

  bool _bQuit = false;
  typedef std::vector<char *> Strings;
  Strings _childCommands, _fdLinks;

private:
  static std::string _unlink(int fdBy, Xtee::FDIndex &lookup, Xtee::FDIndex &reverseLookup);

  fd_set _fdsetRead, _fdsetErr;

  int64_t _offsetOrigin, _offsetLast;
  int64_t _stampStart, _stampLast;
  int _kBpsLimit, _lastv;
};

#endif // __XTEE_HH__

