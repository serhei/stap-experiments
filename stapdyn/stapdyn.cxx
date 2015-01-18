// stapdyn main program
// Copyright (C) 2012-2014 Red Hat Inc.
// Copyright (C) 2013-2014 Serhei Makarov
//
// This file is part of systemtap, and is free software.  You can
// redistribute it and/or modify it under the terms of the GNU General
// Public License (GPL); either version 2, or (at your option) any
// later version.

// TODOXXX TEMPORARY EXPEDIENT FOR TESTING UPDATED stapdyn MULTIPLEXER
#define USE_MULTIDYN

#include <iostream>
#include <memory>

#include <boost/shared_ptr.hpp>

extern "C" {
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <wordexp.h>
}

#include "config.h"
#include "../git_version.h"
#include "../version.h"

#include "mutator.h"
#include "dynutil.h"


#define _STRINGIFY(s) #s
#define STRINGIFY(s) _STRINGIFY(s)
#define DYNINST_FULL_VERSION \
  ( STRINGIFY(DYNINST_MAJOR) "." \
    STRINGIFY(DYNINST_MINOR) "." \
    STRINGIFY(DYNINST_SUBMINOR) )


using namespace std;


// Print the obligatory usage message.
static void __attribute__ ((noreturn))
usage (int rc)
{
  cout << "Usage: " << program_invocation_short_name
       << " MODULE [-v] [-c CMD | -x PID] [-o FILE] [-C WHEN] [globalname=value ...] [-V] [-h]" << endl
#ifdef USE_MULTIDYN
       << "   or: " << program_invocation_short_name
       << " -M MULTIMODULES" << endl
#endif
       << "-v              Increase verbosity." << endl
       << "-c cmd          Command \'cmd\' will be run and " << program_invocation_short_name << " will" << endl
       << "                exit when it does.  The '_stp_target' variable" << endl
       << "                will contain the pid for the command." << endl
       << "-x pid          Sets the '_stp_target' variable to pid." << endl
       << "-o FILE         Send output to FILE. This supports strftime(3)" << endl
       << "                formats for FILE." << endl
       << "-C WHEN         Enable colored errors. WHEN must be either 'auto'," << endl
       << "                'never', or 'always'. Set to 'auto' by default." << endl
#ifdef USE_MULTIDYN
       << "-M MULTIMODULES Run multiple modules (file contains list of stapdyn command lines)." << endl
#endif
       << "-V              Show version." << endl
       << "-h              Show this help text." << endl;

  exit (rc);
}


#ifndef USE_MULTIDYN
// This is main, the heart and soul of stapdyn, oh yeah!
int
main(int argc, char * const argv[])
{
  pid_t pid = 0;
  const char* command = NULL;
  const char* module = NULL;

  // Check if error/warning msgs should be colored
  color_errors = isatty(STDERR_FILENO)
    && strcmp(getenv("TERM") ?: "notdumb", "dumb");

  // First, option parsing.
  int opt;
  while ((opt = getopt (argc, argv, "c:x:vwo:VhC:")) != -1)
    {
      switch (opt)
        {
        case 'c':
          command = optarg;
          break;

        case 'x':
          pid = atoi(optarg);
          break;

        case 'v':
          ++stapdyn_log_level;
          break;

        case 'w':
          stapdyn_suppress_warnings = true;
          break;

        case 'o':
          stapdyn_outfile_name = optarg;
          break;

        case 'V':
          printf("Systemtap Dyninst loader/runner (version %s/%s, %s)\n"
                 "Copyright (C) 2012-2014 Red Hat, Inc. and others\n"
                 "This is free software; see the source for copying conditions.\n",
                 VERSION, DYNINST_FULL_VERSION, STAP_EXTENDED_VERSION);
          return 0;
        case 'h':
          usage(0);
          break;
        case 'C':
          if (!strcmp(optarg, "never"))
            color_errors = false;
          else if (!strcmp(optarg, "auto"))
            color_errors = isatty(STDERR_FILENO)
              && strcmp(getenv("TERM") ?: "notdumb", "dumb");
          else if (!strcmp(optarg, "always"))
            color_errors = true;
          else {
            staperror() << "Invalid option '" << optarg << "' for -C." << endl;
            usage (1);
          }
          break;
        default:
          usage (1);
        }
    }

  // The first non-option is our stap module, required.
  if (optind < argc)
    module = argv[optind++];

  // Remaining non-options, if any, specify global variables.
  vector<string> modoptions;
  while (optind < argc)
    {
      modoptions.push_back(string(argv[optind++]));
    }

  if (!module || (command && pid))
    usage (1);

  // TODOXXX should contact or fork a server process here

  // Make sure that environment variables and selinux are set ok.
  if (!check_dyninst_rt())
    return 1;
  if (!check_dyninst_sebools(pid != 0))
    return 1;

  auto_ptr<mutator> session(new mutator());
  if (!session.get())
    {
      staperror() << "Failed to initialize dyninst session!" << endl;
    }
  boost::shared_ptr<script_module> script
    = session->create_module(module, modoptions);
  if (!script.get() || !script->load())
    {
      staperror() << "Failed to load script module!" << endl;
    }

  if (command && !script->set_main_target(session->create_process(command)))
    return 1;

  if (pid && !script->set_main_target(session->attach_process(pid)))
    return 1;

  if (!script->start())
    return 1;

  // TODOXXX in the server, this should be an event loop accepting connections
  if (!session->run_to_completion())
    return 1;

  return script->exit_status();
}
#else
// if defined USE_MULTIDYN

boost::shared_ptr<script_module>
launch_module(mutator *session, int argc, char * const argv[])
{
#define NULLP boost::shared_ptr<script_module>()

  pid_t pid = 0;
  const char* command = NULL;
  const char* module = NULL;

  // First, option parsing.
  int opt;
  while ((opt = getopt (argc, argv, "c:x:vwo:VhC:")) != -1)
    {
      switch (opt)
        {
        case 'c':
          command = optarg;
          break;

        case 'x':
          pid = atoi(optarg);
          break;

        case 'v':
          ++stapdyn_log_level;
          break;

        case 'w':
          stapdyn_suppress_warnings = true;
          break;

        case 'o':
          stapdyn_outfile_name = optarg;
          break;

        case 'V':
          printf("Systemtap Dyninst loader/runner (version %s/%s, %s)\n"
                 "Copyright (C) 2012-2014 Red Hat, Inc. and others\n"
                 "This is free software; see the source for copying conditions.\n",
                 VERSION, DYNINST_FULL_VERSION, STAP_EXTENDED_VERSION);
          return NULLP;
        case 'h':
          usage(0);
          break;
        case 'C':
          if (!strcmp(optarg, "never"))
            color_errors = false;
          else if (!strcmp(optarg, "auto"))
            color_errors = isatty(STDERR_FILENO)
              && strcmp(getenv("TERM") ?: "notdumb", "dumb");
          else if (!strcmp(optarg, "always"))
            color_errors = true;
          else {
            staperror() << "Invalid option '" << optarg << "' for -C." << endl;
            usage (1);
          }
          break;
        default:
          usage (1);
        }
    }

  // The first non-option is our stap module, required.
  if (optind < argc)
    module = argv[optind++];

  // Remaining non-options, if any, specify global variables.
  vector<string> modoptions;
  while (optind < argc)
    {
      modoptions.push_back(string(argv[optind++]));
    }

  if (!module || (command && pid))
    usage (1);

  // Make sure that environment variables and selinux are set ok.
  if (!check_dyninst_sebools(pid != 0))
    return NULLP;

  boost::shared_ptr<script_module> script
    = session->create_module(module, modoptions);
  if (!script.get() || !script->load())
    {
      staperror() << "Failed to load script module!" << endl;
      return NULLP;
    }

  if (command && !script->set_main_target(session->create_process(command)))
    return NULLP;

  if (pid && !script->set_main_target(session->attach_process(pid)))
    return NULLP;

  if (!script->start())
    return NULLP;

  return script;
}

int main(int argc, char * const argv[])
{
  // Check if error/warning msgs should be colored
  color_errors = isatty(STDERR_FILENO)
    && strcmp(getenv("TERM") ?: "notdumb", "dumb");

  // Make sure that environment variables and selinux are set ok.
  if (!check_dyninst_rt())
    return 1;

  auto_ptr<mutator> session(new mutator());
  if (!session.get())
    {
      staperror() << "Failed to initialize dyninst session!" << endl;
    }

  // Check for '-M' option asking us to run multi-modules:
  if (argc > 2 /* TODOXXX */ && strcmp(argv[1], "-M") == 0)
    {
      FILE *cmdlines = fopen(argv[2], "r");
      if (cmdlines == NULL)
        {
          staperror() << "File not found: " << string(argv[2]) << endl;
        }

      // Read and launch a script for each command line:
#define BUF_MAX 200
      char buf[BUF_MAX];
      while (fgets(buf, BUF_MAX, cmdlines) != NULL)
        {
          // Get rid of newline for wordexp:
          for (int i = 0; buf[i] != '\0'; i++)
            if (buf[i] == '\n') buf[i] = 0;

          // Split the command into words:
          int child_argc; char* const* child_argv; wordexp_t words;
          int rc = wordexp (buf, &words, WRDE_NOCMD|WRDE_UNDEF);
          if (rc != 0)
            {
              staperror() << "wordexp parsing error (" << rc << ") in '"
                          << string(buf) << "'" << endl;
              return 1;
            }

          child_argc = (int) words.we_wordc;
          child_argv = (/*cheater*/ char* const*) words.we_wordv;

          clog << "launching '" << string(buf) << "'";
          boost::shared_ptr<script_module> script
            = launch_module(session.get(), child_argc, child_argv);
          if (!script.get())
            clog << " -- launch failed";
          clog << endl;
        }

      fclose(cmdlines);

      if (!session->run_to_completion())
        return 1;

      return 0;
    }

  // Otherwise, just run a single module:
  boost::shared_ptr<script_module> script
    = launch_module(session.get(), argc, argv);
  if (!script.get())
    return 1;

  if (!session->run_to_completion())
    return 1;

  return script->exit_status();
}

#endif

/* vim: set sw=2 ts=8 cino=>4,n-2,{2,^-2,t0,(0,u0,w1,M1 : */
