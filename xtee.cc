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
int Xtee::errlog(unsigned int category, const char *fmt, ...)
{
  if (0 == (_options.logflags & category))
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

Xtee::Xtee()
    : _options({.noOutFile = false,
                .append = false,
                .bitrate = -1,
                .bytesToSkip = -1,
                .secsToSkip = -1,
                .secsDuration = -1,
                .secsTimeout = -1,
                .logflags = 0xff})
{
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

// -----------------------------
// procfd()
// -----------------------------
char buf[1024] = {0};
int Xtee::procfd(int &fd, fd_set &fdread, fd_set &fderr, int defaultfd, int childIdx)
{
  int n = 0;

  if (IS_VALID_FLAG_SET(fd, fdread) && (n = ::read(fd, buf, sizeof(buf))) > 0)
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

  if (fd > STDERR_FILENO && IS_VALID_FLAG_SET(fd, fderr))
  {
    errlog(LOGF_TRACE, "CH%02u[%s] closing error fd(%d)", childIdx, fd);
    ::close(fd);
    fd = -1;
    n = -1;
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

  errlog(LOGF_TRACE, "closed link(s) to C%02d[%s]: %s", child.idx, child.cmd, batch.c_str());
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
      errlog(LOGF_TRACE, "CH%02u[%s] spawned: %d<%d, %d>%d, %d>%d", i + 1, childcmd,
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

      for (size_t c = 0; c < _children.size(); c++)
      {
        ChildStub &child = _children[c];
        for (int j = 0; j < 3; j++)
          ::close(child.stdio[j]);
      }

      _children.clear();

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
      errlog(LOGF_TRACE, "CH%02u[%s] starts", i + 1, cmdline.c_str());
      int ret = execvp(childargv[0], childargv);

      // child step 4. exitting
      errlog(LOGF_TRACE, "CH%02u[%s] exited (%d)", i + 1, cmdline.c_str(), ret);
      fsync(STDOUT_FILENO);
      fsync(STDERR_FILENO);

      exit(ret); // end of the child process
    }

    // this is the parent process
    if (pidChild <= 0)
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
    child.stdio[0] = child.stdio[1] = child.stdio[2] = -1;
    ::close(PSTDIN(stdioPipes)[0]);
    child.stdio[0] = PSTDIN(stdioPipes)[1];
    ::close(PSTDOUT(stdioPipes)[1]);
    child.stdio[1] = PSTDOUT(stdioPipes)[0];
    ::close(PSTDERR(stdioPipes)[1]);
    child.stdio[2] = PSTDERR(stdioPipes)[0];

    for (int j = 0; j < 3; j++)
      ::fcntl(child.stdio[j], F_SETFL, O_NONBLOCK);

    _children.push_back(child);
    errlog(LOGF_TRACE, "CH%02u[%s] created: pid(%d) %d>%d, %d<%d, %d<%d", child.idx, child.cmd, child.pid,
           PSTDIN(stdioPipes)[0], PSTDIN(stdioPipes)[1], PSTDOUT(stdioPipes)[0], PSTDOUT(stdioPipes)[1], PSTDERR(stdioPipes)[0], PSTDERR(stdioPipes)[1]);
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
      errlog(LOGF_ERROR, "skip invalid link CH%02d:%d <- CH%02d:%d", childIdDest, childFdDest, childIdSrc, childFdSrc);
      continue;
    }

    int destPipe = (childIdDest > 0) ? _children[childIdDest - 1].stdio[0] : STDIN_FILENO;

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

    errlog(LOGF_TRACE, "linked (%d)CH%02d:%d <- (%d)CH%02d:%d", destPipe, childIdDest, childFdDest, srcPipe, childIdSrc, childFdSrc);
  }

  // scan and fulfill the orphan pipes to the parent
  for (size_t i = 0; i < _children.size(); i++) // -- disabled
  {
    ChildStub &child = _children[i];

    // link the undefine pipes to the parent
    if (_fd2src.end() == _fd2src.find(CHILDIN(child)))
    {
      link(STDIN_FILENO, CHILDIN(child));
      errlog(LOGF_TRACE, "linked (%d)CH%02d:IN <- PA:IN", CHILDIN(child), i+1);
    }

    if (_fd2fwd.end() == _fd2fwd.find(CHILDOUT(child)))
    {
      link(CHILDOUT(child), STDOUT_FILENO);
      errlog(LOGF_TRACE, "linked (%d)CH%02d:OUT -> PA:OUT", CHILDOUT(child), i+1);
    }

    if (_fd2fwd.end() == _fd2fwd.find(CHILDERR(child)))
    {
      link(CHILDERR(child), STDERR_FILENO);
      errlog(LOGF_TRACE, "linked (%d)CH%02d:ERR -> PA:OUT", CHILDERR(child), i+1);
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

  for (int timeouts = 0; !_bQuit && (maxTimeouts < 0 || timeouts < maxTimeouts);)
  {
    // pa step 5.1 check the child processes
    if (bChildCheckNeeded || nIdles > (10000 / CHECK_INTERVAL))
    {
      nIdles = 0;
      for (size_t j = 0; !_bQuit && j < _children.size(); j++)
      {
        ChildStub &child = _children[j];
        pid_t wpid = waitpid(child.pid, &child.status, WNOHANG | WUNTRACED); // instantly return
        if (wpid != 0)                                                       // WNOHANG returns 0 when child is still running
        {
          closePipesToChild(child);
          if (wpid == child.pid)
            errlog(LOGF_TRACE, "CH%02u[%s] exited: pid(%d) status(0x%x)", child.idx, child.cmd, child.pid, child.status);
          else errlog(LOGF_TRACE, "CH%02u[%s] gone: pid(%d)", child.idx, child.cmd, child.pid);
        }
      }
    }

    // pa step 5.2 prepare fdset for select()
    int bytesChildrenIO = 0;

    fd_set fdread, fderr;
    FD_ZERO(&fdread);
    FD_ZERO(&fderr);

    int maxfd = -1;
    for (FDIndex::iterator it = _fd2fwd.begin(); it != _fd2fwd.end(); it++)
    {
      if (it->first < 0)
        continue;

      FD_SET(it->first, &fdread);
      FD_SET(it->first, &fderr);
      if (maxfd < it->first)
        maxfd = it->first;
    }

    if (maxfd <= STDERR_FILENO && _children.size() > 0)
    {
      // no child seems alive, quit
      _bQuit = true;
      break;
    }

    // pa step 5.3 do select()
    struct timeval timeout;
    timeout.tv_sec = CHECK_INTERVAL / 1000;
    timeout.tv_usec = (CHECK_INTERVAL % 1000) * 1000;

    int rc = select(maxfd + 1, &fdread, NULL, &fderr, &timeout);
    if (_bQuit)
      break;

    // pa step 5.4  select() dispatching
    if (rc < 0) // select() err, quit
    {
      errlog(LOGF_ERROR, "io wait got (%d) error(%d), quiting", rc, errno);
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
      ssize_t n = procfd(fdStdin, fdread, fderr, STDOUT_FILENO);

      if (n <= 0 && _children.empty())
        break;

      if (n > 0)
      {
        // TODO: bitrate control
      }
    }

    // pa step 5.6 scan if any child has IO occured
    for (size_t i = 0; !_bQuit && i < _children.size(); i++)
    {
      ChildStub &child = _children[i];
      ssize_t n = 0;

      // about the child's stdout
      n = procfd(CHILDOUT(child), fdread, fderr, STDOUT_FILENO, child.idx);

      if (n < 0)
        bChildCheckNeeded = true;
      else
        bytesChildrenIO += n;

      // about the child's stderr
      n = procfd(CHILDERR(child), fdread, fderr, STDERR_FILENO, child.idx);
      if (n < 0)
        bChildCheckNeeded = true;
      else
        bytesChildrenIO += n;
    }

    if (bytesChildrenIO <= 0)
      nIdles++;

  } // end of select() loop

  // pa step 6. close all pipes that are still openning
  errlog(LOGF_TRACE, "end of loop, cleaning up %u child(s)", _children.size());
  for (size_t i = 0; i < _children.size(); i++)
    closePipesToChild(_children[i]);

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

  ::fsync(fdSrc);
  ::close(fdSrc);
  fdSrc =-1;

  return batch;
}

std::string Xtee::closeDestFd(int& fdDest)
{
  std::string batch = fd2str(fdDest) + "<-[" + _unlink(fdDest, _fd2src, _fd2fwd) +"]";
  ::fsync(fdDest);
  ::close(fdDest);
  fdDest =-1;

  return batch;
}
