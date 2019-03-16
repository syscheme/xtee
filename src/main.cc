#include "xtee.hh"
extern "C"
{
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
}

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
            << "  -s <kbps>            limits the transfer bitrate at reading from stdin in kbps" EOL
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

  Xtee xtee;

  int opt = 0;
  while (-1 != (opt = getopt(argc, argv, "hnas:k:t:d:q:c:l:")))
  {
    switch (opt)
    {
    case 'n':
      xtee._options.noOutFile = true;
      break;

    case 'a':
      xtee._options.append = true;
      break;

    case 's':
      xtee._options.kbps = atol(optarg);
      break;

    case 'k':
      xtee._options.bytesToSkip = atol(optarg);
      break;

    case 't':
      xtee._options.secsToSkip = atoi(optarg);
      break;

    case 'd':
      xtee._options.secsDuration = atoi(optarg);
      break;

    case 'q':
      xtee._options.secsTimeout = atoi(optarg);
      break;

    case 'c':
      xtee.pushCommand(optarg);
      break;

    case 'l':
      xtee.pushLink(optarg);
      break;

    default:
      ret = -1;
    case 'h':
    case '?':
      usage();
      return ret;
    }
  }

  return xtee.init() ? xtee.run() :-100;
}
