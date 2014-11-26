// stapdyn mutator functions
// Copyright (C) 2012-2013 Red Hat Inc.
// Copyright (C) 2013-2014 Serhei Makarov
//
// This file is part of systemtap, and is free software.  You can
// redistribute it and/or modify it under the terms of the GNU General
// Public License (GPL); either version 2, or (at your option) any
// later version.

#ifndef MUTATOR_H
#define MUTATOR_H

#include <string>
#include <vector>

#include <boost/shared_ptr.hpp>

#include <BPatch.h>
#include <BPatch_module.h>
#include <BPatch_process.h>
#include <BPatch_thread.h>

#include "dynprobe.h"
#include "dynutil.h"
#include "mutatee.h"
extern "C" {
#include "../runtime/dyninst/stapdyn.h"
}

/* An Outline of How the Multiplexing Extension Works

   In order to allow multiple systemtap script modules which might
   instrument multiple target processess, we need to manage script
   modules and mutatees separately.

   - mutator :: single object managing the Dyninst session
     - OWNS SEVERAL mutatee :: one object per target process
       - OWNS SEVERAL script_target (jointly with script_module)
     - OWNS SEVERAL script_module :: one object per running Systemtap module 
       - OWNS SEVERAL script_target (jointly with mutatee)

   At the bottom of the hierarchy is the script_target class. A
   script_target represents a specific snippet of code (owned by a
   single Systemtap module) that we can load and unload from a single
   target process. Each script_target is thus jointly owned by one
   mutatee object and one script_module object.

   For global Dyninst callbacks that need to be managed by the
   mutator, we use a reference counting mechanism: each script_module
   specifies what types of callbacks it wants (p_need_*), and the
   mutator maintains a count of how many of the currently running
   script_modules want each callback (c_need_*). If any count reaches
   zero, the corresponding callback is disabled.

 */


class script_module;


// The mutator "owns" the global Dyninst session and drives all instrumentation.
class mutator {
  private:
    BPatch patch;

    std::vector<boost::shared_ptr<script_module> > script_modules; // all running stapdyn modules
    std::vector<boost::shared_ptr<mutatee> > mutatees; // all attached target processes

    // Since callbacks provided by Dyninst are configured globally for
    // all script_modules, we need reference counts to track how many
    // script_modules need each one:
    int c_need_callbacks;
    int c_need_exit;
    int c_need_thread_create;
    int c_need_thread_destroy;
    // When any of these reach zero, the corresponding callback is disabled.

    sigset_t signals_received; // record all signals we've caught

    // disable implicit constructors by not implementing these
    mutator (const mutator& other);
    mutator& operator= (const mutator& other);

    // Check the status of all mutatees and script_modules.
    bool update_modules(bool *store_success);

    // Find a mutatee which matches the given process, else return NULL
    boost::shared_ptr<mutatee> find_mutatee(BPatch_process* process);
    boost::shared_ptr<mutatee> find_mutatee(pid_t pid);

  public:
    mutator ();
    ~mutator ();

    // Create a new script_module to be handled by this mutator.
    boost::shared_ptr<script_module> create_module
      (const std::string& module_name, std::vector<std::string>& module_options);

    // Wait for all currently running script_modules to complete.
    bool run_to_completion ();

    // Ensure required callbacks for a script_module:
    void register_callbacks (bool need_exit, bool need_thread_create,
                             bool need_thread_destroy);

    // Remove callbacks if no further script_modules need them:
    void remove_callbacks (bool need_exit, bool need_thread_create,
                           bool need_thread_destroy);

    // Create a new process with the given command line.
    boost::shared_ptr<mutatee> create_process(const std::string& command);

    // Obtain mutatee for a specific existing process.
    boost::shared_ptr<mutatee> attach_process(BPatch_process* process);
    boost::shared_ptr<mutatee> attach_process(pid_t pid);

    // Callback to respond to dynamically loaded libraries.
    // Check if it matches our targets, and instrument accordingly.
    void dynamic_library_callback(BPatch_thread *thread,
                                  BPatch_module *module,
                                  bool load);

    // Callback to respond to post fork events.  Check if it matches
    // our targets, and handle accordingly.
    void post_fork_callback(BPatch_thread *parent, BPatch_thread *child);
    void exec_callback(BPatch_thread *thread);
    void exit_callback(BPatch_thread *thread, BPatch_exitType type);

    void thread_create_callback(BPatch_process *proc, BPatch_thread *thread);
    void thread_destroy_callback(BPatch_process *proc, BPatch_thread *thread);

    // Callback to respond to signals.
    void signal_callback(int signal);
};

// A script_module manages a single stap .so module
class script_module {
  private:
    mutator *owner;

    void* module; // the locally dlopened probe module
    std::string module_name; // the filename of the probe module
    std::vector<std::string> modoptions; // custom globals from -G option
    std::string module_shmem; // the global name of this module's shared memory
    std::vector<dynprobe_target> targets; // the probe targets in the module

    std::vector<boost::shared_ptr<script_target> > script_targets; // snippets for all processes to instrument
    boost::shared_ptr<script_target> main_target; // the main target process we created or attached
    bool p_target_created; // we only kill and wait on the target we created
    bool p_target_error; // indicates whether the target exited non-zero;

    // Flags to track which callbacks the mutator needs to load for us
    bool p_callbacks_enabled;
    bool p_need_exit;
    bool p_need_thread_create;
    bool p_need_thread_destroy;

    // disable implicit constructors by not implementing these
    script_module (const script_module& other);
    script_module& operator= (const script_module& other);

    // Initialize the module global variables
    bool init_modoptions();

    // Initialize the session attributes
    void init_session_attributes();

    // Initialize the module session
    bool run_module_init();

    // Shutdown the module session
    bool run_module_exit();

    // Do probes matching 'flag' exist?
    bool matching_probes_exist(uint64_t flag);

    // Find a script_target which matches the given process, else return NULL
    boost::shared_ptr<script_target> find_target(BPatch_process* process);
    boost::shared_ptr<script_target> find_target(pid_t pid);

    // Stashed utrace probe enter function pointer.
    typeof(&enter_dyninst_utrace_probe) utrace_enter_fn;

  public:
    script_module (mutator *owner, const std::string& module_name,
                   std::vector<std::string>& module_options);
    ~script_module ();

    // Load the stap module and initialize all probe info.
    bool load ();

    // Set the main target of the stap module.
    bool set_main_target(boost::shared_ptr<mutatee> mut);
    boost::shared_ptr<script_target> get_main_target() { return main_target; }

    // Create a new process with the given command line.
    bool create_process(const std::string& command);

    // Attach to a specific existing process.
    bool attach_process(BPatch_process* process);
    bool attach_process(pid_t pid);

    // Start the actual systemtap session!
    bool start ();

    // Shut down the systemtap session.
    bool stop ();

    // Get the final exit status of this module.
    int exit_status();

    // Callback to respond to dynamically loaded libraries.
    // Check if it matches our targets, and instrument accordingly.
    void dynamic_library_callback(BPatch_thread *thread,
                                  BPatch_module *module,
                                  bool load);

    // Callback to respond to post fork events.  Check if it matches
    // our targets, and handle accordingly.
    void post_fork_callback(mutatee *parent, mutatee *child,
                            BPatch_thread *child_thread);
    void exec_callback(BPatch_thread *thread);
    void exit_callback(BPatch_thread *thread, BPatch_exitType type);
};


#endif // MUTATOR_H

/* vim: set sw=2 ts=8 cino=>4,n-2,{2,^-2,t0,(0,u0,w1,M1 : */
