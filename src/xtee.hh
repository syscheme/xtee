#ifndef __XTEE_HH__
#define __XTEE_HH__

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
#define QoS_MEASURES_PER_SEC      (10)  // 10 times per second

#define LOGF_TRACE (1 << 0)
#define LOGF_ERROR (1 << 1)

// -----------------------------
// class Xtee
// -----------------------------
// an extersion to unix command tee to cover:
//   - spawn multiple child processes
//   - link the stdin/stdout/stderr of the children and this xtee
//   - exchange the stdxx stream among the parties
//   - QoS control thru xtee's stdin-to-stdout, covers speed control, scheduling, bytes-to-skip, etc.
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
  int  run();

  void stop() { _bQuit =true; }

  int pushCommand(char* cmd);
  int pushLink(char* link);

  int  errlog(unsigned short category, const char *fmt, ...);
  void printLinks();

  static int lineToArgv(char* argv[], int maxargc, char *line, int linelen);

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
  
  //@return bytes read from the fd, -1 if error occured at reading
  int     checkAndForward(int &fd, int defaultfd, int childIdx = -1);
  void    closePipesToChild(ChildStub &child);
  int     stdinQoS(const char* buf, int len);

  bool _bQuit = false;
  typedef std::vector<char *> Strings;
  Strings _childCommands, _fdLinks;

private:
  static std::string _unlink(int fdBy, Xtee::FDIndex &lookup, Xtee::FDIndex &reverseLookup);

  fd_set _fdsetRead, _fdsetErr;

  int64_t _stampStart, _stampLast;
  int64_t _offsetOrigin, _offsetLast;
  int _kBpsLimit, _lastv;
  int _childsToStdin;

public:
  Options _options;
};

#endif // __XTEE_HH__

