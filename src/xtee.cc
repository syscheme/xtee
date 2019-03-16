#include "xtee.hh"

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

#define PSTDIN(_PIO) (_PIO[0])
#define PSTDOUT(_PIO) (_PIO[1])
#define PSTDERR(_PIO) (_PIO[2])

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
// errlog()
// -----------------------------
#define LOG_LINE_MAX_BUF (256)
int Xtee::errlog(unsigned short category, const char *fmt, ...)
{
  if (0 == (_options.logflags & category))
    return 0;

  char msg[LOG_LINE_MAX_BUF] ="", *p=msg;
  p += snprintf(p, LOG_LINE_MAX_BUF -20, "\nxtee[%02x]: ", category);
  va_list args;

  va_start(args, fmt);
  p += ::vsnprintf(p, msg + LOG_LINE_MAX_BUF - p -8, fmt, args);
  va_end(args);
  *p++ = '\r';  *p++ = '\n';  *p++ = '\0';

  return ::write(STDERR_FILENO, msg, p-msg);
}

static int64_t now()
{
  struct timeval tmval;
  if (0 != gettimeofday(&tmval, (struct timezone *)NULL))
    return 0;

  return (tmval.tv_sec * 1000LL + tmval.tv_usec / 1000);
}

Xtee::Xtee()
    : _stampStart(0), _stampLast(0), _offsetOrigin(0), _offsetLast(0), _lastv(0), _kBpsLimit(0),
    _options({.noOutFile = false,
                .append = false,
                .kbps = -1,
                .bytesToSkip = -1,
                .secsToSkip = -1,
                .secsDuration = -1,
                .secsTimeout = -1,
                .logflags = 0xff})
{
}

bool Xtee::init()
{
  if (_options.secsToSkip >0)
    _stampStart = _options.secsToSkip *1000 + now();

  if (_options.kbps >0)
    _kBpsLimit = _options.kbps >>3;
}

int Xtee::pushCommand(char *cmd)
{
  if (cmd && strlen(cmd) > 0)
    _childCommands.push_back(cmd);

  return _childCommands.size();
}

int Xtee::pushLink(char *link)
{
  if (link && strlen(link) > 0)
    _fdLinks.push_back(link);

  return _fdLinks.size();
}

static char buf[1024] = {0};
// -----------------------------
// checkAndForward()
// -----------------------------
//@return bytes read from the fd, -1 if error occured at reading
int Xtee::checkAndForward(int &fd, int defaultfd, int childIdx)
{
  int n = 0;

  if (IS_VALID_FLAG_SET(fd, _fdsetRead) && (n = ::read(fd, buf, sizeof(buf))) > 0)
  {
    FDIndex::iterator itIdx = _fd2fwd.find(fd);
    if (_fd2fwd.end() == itIdx) // if (fwdset.empty())
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
      FDSet& fwdset =itIdx->second;
      for (FDSet::const_iterator it = fwdset.begin(); it != fwdset.end(); it++)
      {
        if (*it > 0)
          ::write(*it, buf, n);
      }
    }
  }

  if (fd > STDERR_FILENO && IS_VALID_FLAG_SET(fd, _fdsetErr))
  {
    errlog(LOGF_TRACE, "closing damaged-fd(%d) to CH%02u", fd, childIdx);
    closeSrcFd(fd);
    n = -1;
  }

  return n;
}

// -----------------------------
// stdinQoS()
// -----------------------------
int Xtee::stdinQoS()
{
  if (!IS_VALID_FLAG_SET(STDIN_FILENO, _fdsetRead))
    return 0;

  int n = ::read(STDIN_FILENO, buf, sizeof(buf));
  if (n <= 0)
    return n;

  int64_t stampNow = now();

  // yield if stamp start is defined and not meet
  if (_stampStart >0 && _stampStart > stampNow)
    return n;
  if (_stampStart<=0)
    _stampStart = stampNow;

  if (_options.secsDuration >0 && stampNow >(_stampStart + _options.secsDuration *1000))
    _bQuit = true;

  char * p = buf;
  
  // yield if the leading yield bytes is specified
  if (_options.bytesToSkip >0)
  {
    if ((_offsetOrigin +n) <= _options.bytesToSkip)
      return n;
  
    if (_offsetOrigin <_options.bytesToSkip)
    {
      uint bytesToSkip = _options.bytesToSkip - _offsetOrigin;
      p += bytesToSkip;
      n -= bytesToSkip;
    }
  }

  _offsetOrigin += n;

  // forward the data
  FDIndex::iterator itIdx = _fd2fwd.find(STDIN_FILENO);
  if (_fd2fwd.end() == itIdx) // if (fwdset.empty())
    ::write(STDOUT_FILENO, p, n);
  else
  {
    FDSet &fwdset = itIdx->second;
    for (FDSet::const_iterator it = fwdset.begin(); it != fwdset.end(); it++)
    {
      if (*it > 0)
        ::write(*it, p, n);
    }
  }

  // limit the speed
  if (_options.kbps > 0)
  {
    stampNow = now();
    if (_stampLast <= 0)
    {
      _stampLast = stampNow;
      _offsetLast = _offsetOrigin;
    }

    int elapsed = stampNow - _stampLast;
    int msecYield =0;
    if(_kBpsLimit > 0 && elapsed > 300)
    {
      int msecP = (_offsetOrigin - _offsetLast) / _kBpsLimit - elapsed;
      int msecI = (_offsetOrigin - _options.bytesToSkip) / _kBpsLimit - (stampNow - _stampStart);
      int v = (_offsetOrigin - _offsetLast) / elapsed;
      int msecV = (v > _lastv) ? 1 : -1;

      msecYield = msecP + (msecI / 4) + msecV;
      
      _lastv = v;
      _stampLast = stampNow;
    }

    if (msecYield > 0)
      ::usleep(msecYield * 1000);
  }

  return n;
}

// -----------------------------
// closePipesToChild()
// -----------------------------
void Xtee::closePipesToChild(ChildStub &child)
{
  // int nClosed = 0;
  // for (int j = 0; j < 3; j++)
  // {
  //   int &fd = child.stdio[j];
  //   if (fd > STDERR_FILENO)
  //   {
  //     ::fsync(fd);
  //     ::close(fd);
  //     fd = -1;
  //     nClosed++;
  //   }
  // }

  // for (FDSet::iterator it = child.fwdStdout.begin(); it != child.fwdStdout.end(); it++)
  // {
  //   if (*it > STDERR_FILENO)
  //     ::close(*it);
  // }

  // child.fwdStdout.clear();

  // for (FDSet::iterator it = child.fwdStderr.begin(); it != child.fwdStderr.end(); it++)
  // {
  //   if (*it > STDERR_FILENO)
  //     ::close(*it);
  // }

  // child.fwdStderr.clear();

  std::string batch;

  if (CHILDIN(child) > STDERR_FILENO)
    batch += closeDestFd(CHILDIN(child)) + ","; // STDIN of the chhild

  if (CHILDOUT(child) > STDERR_FILENO)
    batch += closeSrcFd(CHILDOUT(child)) + ","; // STDOUT of the chhild

  if (CHILDERR(child) > STDERR_FILENO)
    batch += closeSrcFd(CHILDERR(child)); // STDERR of the chhild

  errlog(LOGF_TRACE, "closed link(s) of CH%02d[%s]: %s", child.idx, child.cmd, batch.c_str());
  // return nClosed;
}

// -----------------------------
// run()
// -----------------------------
int Xtee::run()
{
  for (size_t i = 0; i < _childCommands.size(); i++)
  {
    char *childcmd = _childCommands[i];

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
      errlog(LOGF_TRACE, "CH%02u[%d<%d,%d>%d,%d>%d] spawned: %s", i + 1, 
            PSTDIN(stdioPipes)[0], PSTDIN(stdioPipes)[1], PSTDOUT(stdioPipes)[1], PSTDOUT(stdioPipes)[0], PSTDERR(stdioPipes)[1], PSTDERR(stdioPipes)[0],
            childcmd);

      std::string cmdline = childcmd;

      // this the child process
      // child step 1. remap the pipe to local stdXX
      ::dup2(PSTDIN(stdioPipes)[0], STDIN_FILENO);
      ::close(PSTDIN(stdioPipes)[0]), ::close(PSTDIN(stdioPipes)[1]);
      ::dup2(PSTDOUT(stdioPipes)[1], STDOUT_FILENO);
      ::close(PSTDOUT(stdioPipes)[0]), ::close(PSTDOUT(stdioPipes)[1]);
      ::dup2(PSTDERR(stdioPipes)[1], STDERR_FILENO);
      ::close(PSTDERR(stdioPipes)[0]), ::close(PSTDERR(stdioPipes)[1]);

      for (size_t c = 0; c < _children.size(); c++)
      {
        ChildStub &child = _children[c];
        for (int j = 0; j < 3; j++)
          ::close(child.stdio[j]);
      }

      _children.clear();

      // child step 2. prepare the child command line
      char *childargv[32];
      int childargc = lineToArgv(childargv, sizeof(childargv) / sizeof(childargv[0]) -2, childcmd, strlen(childcmd));
      childargv[childargc] = NULL;

      // child step 3. launch the child command line
      errlog(LOGF_TRACE, "CH%02u executing: %s", i + 1, cmdline.c_str());
      int ret = execvp(childargv[0], childargv);

      // child step 4. child failed to start
      errlog(LOGF_TRACE, "CH%02u quit(%d) err[%s(%d)]: %s", i + 1, ret, strerror(errno), errno, cmdline.c_str());
      fsync(STDOUT_FILENO);
      fsync(STDERR_FILENO);

      exit(ret); // end of the child process
    }

    // this is the parent process
    if (pidChild < 0)
    {
      errlog(LOGF_ERROR, "failed to create CH%02u[%s]: pid(%d)", i, childcmd, pidChild);
      return -100;
    }

    // pa step 3. close the pipe peers and save a stub to the child
    ChildStub child;
    child.idx = i + 1;
    child.cmd = childcmd;
    child.pid = pidChild;
    child.status = 0;
    // file descriptor unused in parent
    CHILDIN(child) = CHILDOUT(child) = CHILDERR(child) = -1;
    ::close(PSTDIN(stdioPipes)[0]);
    CHILDIN(child)  = PSTDIN(stdioPipes)[1];
    ::close(PSTDOUT(stdioPipes)[1]);
    CHILDOUT(child) = PSTDOUT(stdioPipes)[0];
    ::close(PSTDERR(stdioPipes)[1]);
    CHILDERR(child) = PSTDERR(stdioPipes)[0];

    // for (int j = 0; j < 3; j++)
    //   ::fcntl(child.stdio[j], F_SETFL, O_NONBLOCK);

    _children.push_back(child);
    errlog(LOGF_TRACE, "created CH%02u pid(%d) [%d>%d,%d<%d,%d<%d]: %s", child.idx, child.pid,
           CHILDIN(child), PSTDIN(stdioPipes)[0], CHILDOUT(child), PSTDOUT(stdioPipes)[1], CHILDERR(child), PSTDERR(stdioPipes)[1],
           child.cmd);
  }

  // pa step 4. build up the link exchanges
  errlog(LOGF_TRACE, "created %u child(s), making up the links", _children.size());
  for (size_t i = 0; i < _fdLinks.size(); i++)
  {
    char *dest = strtok(_fdLinks[i], ":"), *src = strtok(0, ":"); // char *delimitor = strchr(_fdLinks[i], ':'); // strchr(_fdLinks[i].c_str(), ':');
    if (NULL == dest || NULL == src)
      continue;

    int childIdDest = 0, childFdDest = -1, childIdSrc = 0, childFdSrc = -1;
    char *strChildId = strtok(dest, ".");
    char *strfd = strtok(0, ".");
    if (NULL != strfd)
    {
      childFdDest = atoi(strfd);
      childIdDest = atoi(strChildId);
    }
    else
    {
      childIdDest = atoi(strChildId);
      childFdDest = 0;
    }

    strChildId = strtok(src, ".");
    strfd = strtok(0, ".");
    if (NULL != strfd)
    {
      childFdSrc = atoi(strfd);
      childIdSrc = atoi(strChildId);
    }
    else
    {
      childIdSrc = 0;
      childFdSrc = atoi(strChildId);
    }

    if (childIdDest > (int)_children.size() || childIdSrc > (int)_children.size() || STDIN_FILENO != childFdDest || (childFdSrc != STDOUT_FILENO && childFdSrc != STDERR_FILENO))
    {
      errlog(LOGF_ERROR, "skip invalid link CH%02d:%d<-CH%02d:%d", childIdDest, childFdDest, childIdSrc, childFdSrc);
      continue;
    }

    int destPipe = (childIdDest > 0) ? CHILDIN(_children[childIdDest - 1]) : STDIN_FILENO;
    int srcPipe = (childIdSrc > 0) ? _children[childIdSrc - 1].stdio[childFdSrc] : childFdSrc;

    if (childIdSrc > 0)
    {
      ChildStub &child = _children[childIdSrc - 1];
      // FDSet &fwdset = (childFdSrc == STDOUT_FILENO) ? child.fwdStdout : child.fwdStderr;
      // fwdset.insert(destPipe);
      link((childFdSrc == STDOUT_FILENO)?CHILDOUT(child):CHILDERR(child), destPipe);
    }
    else if (childFdSrc == STDOUT_FILENO)
      // _stdin2fwd.insert(destPipe);
      link(STDIN_FILENO, destPipe);
    else continue;

    errlog(LOGF_TRACE, "linked %d:CH%02d.%d<-%d:CH%02d.%d", destPipe, childIdDest, childFdDest, srcPipe, childIdSrc, childFdSrc);
  }

  // scan and link the orphan pipes to the parent
  for (size_t i = 0; i < _children.size(); i++) // -- disabled
  {
    ChildStub &child = _children[i];

    // link the undefined pipes to the parent
    if (_fd2src.end() == _fd2src.find(CHILDIN(child)))
    {
      link(STDIN_FILENO, CHILDIN(child));
      errlog(LOGF_TRACE, "linked orphan %d:CH%02d.IN<-PA.IN", CHILDIN(child), i+1);
    }

    if (_fd2fwd.end() == _fd2fwd.find(CHILDOUT(child)))
    {
      link(CHILDOUT(child), STDOUT_FILENO);
      errlog(LOGF_TRACE, "linked orphan %d:CH%02d.OUT->PA.OUT", CHILDOUT(child), i+1);
    }

    if (_fd2fwd.end() == _fd2fwd.find(CHILDERR(child)))
    {
      link(CHILDERR(child), STDERR_FILENO);
      errlog(LOGF_TRACE, "linked orphan %d:CH%02d.ERR->PA.ERR", CHILDERR(child), i+1);
    }
  }

  // scan and compress the linkages
  // for (FDIndex::iterator it = _fd2fwd.begin(); it != _fd2fwd.end(); it++)
  // {
  //   if (it->second.size() != 1)
  //     continue;

  //   int dest = (*it->second.begin());
  //   if (dest > STDERR_FILENO)
  //   {
  //     ::dup2(it->first, dest);
  //     src2del.push_back(it->first);
  //     ::close(it->first);
  //     // NOT GOOD
  //   }
  // }

  // pa step 5. start the main loop
  int maxTimeouts = _options.secsTimeout * 1000 / CHECK_INTERVAL;
  bool bChildCheckNeeded = false;
  int nIdles = 0;
  int cLiveChildren =0;

  for (int timeouts = 0; !_bQuit && (maxTimeouts < 0 || timeouts < maxTimeouts);)
  {
    // pa step 5.1 check the child processes
    if (bChildCheckNeeded || nIdles > (10000 / CHECK_INTERVAL))
    {
      cLiveChildren =0;
      nIdles = 0;
      for (size_t j = 0; !_bQuit && j < _children.size(); j++)
      {
        ChildStub &child = _children[j];
        if (child.pid <=0)
          continue;

        pid_t wpid = waitpid(child.pid, &child.status, WNOHANG | WUNTRACED); // instantly return
        if (0 == wpid) // WNOHANG returns 0 when child is still running
        {
          cLiveChildren++;
          continue;
        }

        closePipesToChild(child);
        if (wpid == child.pid)
          errlog(LOGF_TRACE, "detected CH%02u[%s] pid(%d) exited: status(0x%x)", child.idx, child.cmd, child.pid, child.status);
        else
          errlog(LOGF_TRACE, "detected CH%02u[%s] pid(%d) gone", child.idx, child.cmd, child.pid);

        child.pid = -1;
      }
    }

    // pa step 5.2 prepare fdset for select()
    int bytesChildrenIO = 0;

    FD_ZERO(&_fdsetRead);
    FD_ZERO(&_fdsetErr);

    int maxfd = -1;
    for (FDIndex::iterator it = _fd2fwd.begin(); it != _fd2fwd.end(); it++)
    {
      if (it->first < 0)
        continue;

      FD_SET(it->first, &_fdsetRead);
      FD_SET(it->first, &_fdsetErr);
      if (maxfd < it->first)
        maxfd = it->first;
    }

    // errlog(LOGF_TRACE, "loop maxfd[%d] %d/%d live child(s)", maxfd, cLiveChildren,_children.size());
    if (maxfd <= 0) // || cLiveChildren > 0)
    {
      // no child seems alive, quit
      errlog(LOGF_TRACE, "stopping as no more alive child");
      _bQuit = true;
      break;
    }

    // pa step 5.3 do select()
    struct timeval timeout;
    timeout.tv_sec = CHECK_INTERVAL / 1000;
    timeout.tv_usec = (CHECK_INTERVAL % 1000) * 1000;

    int rc = select(maxfd + 1, &_fdsetRead, NULL, &_fdsetErr, &timeout);
    if (_bQuit)
      break;

    // pa step 5.4  select() dispatching
    if (rc < 0) // select() err, quit
    {
      errlog(LOGF_ERROR, "quitting due to io err(%d): %s(%d)", rc, strerror(errno), errno);
      break;
    }
    else if (0 == rc) // timeout
    {
      timeouts++;
      bChildCheckNeeded = true;
      continue;
    }

    // pa step 5.5 about this stdin
    // stdinQoS();
    // {
    //   checkAndForward(STDIN_FILENO, STDOUT_FILENO);

    //   if(cLiveChildren<=0)
    //     break;

    //   QoS((uint)n);
    // }

    // pa step 5.6 scan if any child has IO occured
    for (size_t i = 0; !_bQuit && i < _children.size(); i++)
    {
      ChildStub &child = _children[i];
      ssize_t n = 0;

      // about the child's stdout
      n = checkAndForward(CHILDOUT(child), STDOUT_FILENO, child.idx);

      if (n < 0)
        bChildCheckNeeded = true;
      else
        bytesChildrenIO += n;

      // about the child's stderr
      n = checkAndForward(CHILDERR(child), STDERR_FILENO, child.idx);
      if (n < 0)
        bChildCheckNeeded = true;
      else
        bytesChildrenIO += n;
    }

    if (bytesChildrenIO <= 0)
      nIdles++;

  } // end of select() loop

  // pa step 6. close all pipes that are still openning
  errlog(LOGF_TRACE, "end of loop, cleaning up %u/%u child(s)", cLiveChildren, _children.size());
  for (size_t i = 0; i < _children.size(); i++)
    closePipesToChild(_children[i]);

  ::fsync(STDOUT_FILENO);
  ::fsync(STDERR_FILENO);

  return 0;
}

bool Xtee::link(int fdIn, int fdTo)
{
  if (fdIn < 0 || fdTo < 0)
    return false;

  FDIndex::iterator itIdx = _fd2fwd.find(fdIn);
  if (_fd2fwd.end() == itIdx)
  {
    _fd2fwd.insert(FDIndex::value_type(fdIn, FDSet()));
    itIdx = _fd2fwd.find(fdIn);
  }

  itIdx->second.insert(fdTo);

  itIdx = _fd2src.find(fdTo);
  if (_fd2src.end() == itIdx)
  {
    _fd2src.insert(FDIndex::value_type(fdTo, FDSet()));
    itIdx = _fd2src.find(fdTo);
  }

  itIdx->second.insert(fdIn);
  return true;
}

void Xtee::unlink(int fdIn, int fdTo)
{
  FDIndex::iterator itIdx = _fd2fwd.find(fdIn);
  if (_fd2fwd.end() != itIdx)
    itIdx->second.erase(fdTo);

  if (_fd2src.end() != (itIdx = _fd2src.find(fdTo)))
    itIdx->second.erase(fdIn);
}

static std::string fd2str(int fd)
{
  char buf[10];
  snprintf(buf,sizeof(buf) -2, "%d", fd);
  return buf;
}

void Xtee::printLinks()
{
  std::string result;
  result.reserve(200);
  for (FDIndex::iterator it = _fd2fwd.begin(); it != _fd2fwd.end(); it++)
  {
    std::string to;
    for (FDSet::iterator itSet = it->second.begin(); itSet != it->second.end(); itSet++)
      to += fd2str(*itSet)+ ",";

    if (!to.empty())
      to.erase(to.length() - 1); // erase the last comma
    
    result += fd2str(it->first) + "->[" +to +"];";
  }

  errlog(LOGF_TRACE, "links: ", result.c_str());
}

std::string Xtee::_unlink(int fdBy, Xtee::FDIndex& lookup, Xtee::FDIndex& reverseLookup)
{
  std::string batch;

  FDIndex::iterator itLookup = lookup.find(fdBy);
  if (lookup.end() == itLookup)
    return batch;

  for (FDSet::iterator itInFound = itLookup->second.begin(); itInFound != itLookup->second.end(); itInFound++)
  {
    int fdLinked = *itInFound;
    FDIndex::iterator itReversed = reverseLookup.find(fdLinked);
    if (reverseLookup.end() == itReversed)
      continue; // not found

    itReversed->second.erase(fdBy);
    if (itReversed->second.empty())
    {
      // the fdLinked has no more links left, close it and clean
      ::fsync(fdLinked);
      ::close(fdLinked);
      reverseLookup.erase(fdLinked);
      batch += fd2str(fdLinked) + ",";
    }
  }

  if (!batch.empty())
    batch.erase(batch.length() -1); // erase the last comma

  lookup.erase(fdBy);
  return batch;
}

std::string Xtee::closeSrcFd(int& fdSrc)
{
  std::string batch = fd2str(fdSrc) + "->[" + _unlink(fdSrc, _fd2fwd, _fd2src) +"]";

  if (fdSrc > STDERR_FILENO)
  {
    ::fsync(fdSrc);
    ::close(fdSrc);
    fdSrc = -1;
  }

  return batch;
}

std::string Xtee::closeDestFd(int& fdDest)
{
  std::string batch = fd2str(fdDest) + "<-[" + _unlink(fdDest, _fd2src, _fd2fwd) +"]";
  if (fdDest > STDERR_FILENO)
  {
    ::fsync(fdDest);
    ::close(fdDest);
    fdDest = -1;
  }

  return batch;
}

typedef struct _Token
{
	int posStart, posEnd;
} Token;
typedef std::vector<Token> Tokens;

int Xtee::lineToArgv(char* argv[], int maxargc, char *line, int linelen)
{
	char chBraket =-1;
	Tokens tokens;
	static Token NILTOKEN = {-1, -1};
	Token token = NILTOKEN;

	char* p =line;
	for(p =line; *p && (p-line) <linelen; p++)
	{
		if (!isprint(*p))
		{
			if (token.posStart >=0)
			{
				token.posEnd = (p-line);
				tokens.push_back(token);
				token = NILTOKEN;
			}

			break;
		}

		if (chBraket >0)
		{
			if (chBraket == *p && token.posStart >=0)
			{
				token.posEnd = (p-line);
				tokens.push_back(token);
				chBraket = -1;
				token = NILTOKEN;
			}

			continue;
		}

		if (*p == '"' || *p == '\'' || *p == '`')
		{
			chBraket = *p;
			if (token.posStart >=0)
			{
				token.posEnd = (p-line) -1;
				if (token.posEnd > token.posStart)
					tokens.push_back(token);
				token = NILTOKEN;
			}
			token.posStart = (p-line) +1;
			continue;
		}

		if (isspace(*p))
		{
			if (token.posStart >=0)
			{
				token.posEnd = (p-line);
				tokens.push_back(token);
				token = NILTOKEN;
			}

			continue;
		}

		if (token.posStart < 0)
			token.posStart = (p-line);
	}

	if (token.posStart >= 0)
	{
		token.posEnd = (p - line);
		if (token.posEnd > token.posStart)
			tokens.push_back(token);
		token = NILTOKEN;
	}

	int i=0;
	for (i=0; i < maxargc-1 && i < tokens.size(); i++)
	{
		argv[i] = line + tokens[i].posStart;
		line[tokens[i].posEnd] ='\0';
	}

	argv[i] = NULL;
	return i;
}