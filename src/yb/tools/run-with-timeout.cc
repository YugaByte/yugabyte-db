// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include <signal.h>

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include "yb/gutil/stringprintf.h"
#include "yb/util/subprocess.h"
#include "yb/util/logging.h"
#include "yb/util/status.h"
#include "yb/util/result.h"
#include "yb/util/net/net_util.h"

#ifdef __linux__
#include <sys/resource.h>
#endif

constexpr int kWaitSecAfterSigQuit = 30;
constexpr int kWaitSecAfterSigSegv = 30;

using std::cerr;
using std::condition_variable;
using std::cout;
using std::endl;
using std::invalid_argument;
using std::mutex;
using std::string;
using std::thread;
using std::unique_lock;
using std::vector;
using std::chrono::steady_clock;
using std::chrono::duration_cast;
using std::chrono::milliseconds;

using yb::Subprocess;
using yb::Status;
using yb::Result;
using yb::GetHostname;

bool SendSignalAndWait(Subprocess* subprocess, int signal, const string& signal_str, int wait_sec) {
  LOG(ERROR) << "Timeout reached, trying to stop the child process with " << signal_str;
  const Status signal_status = subprocess->KillNoCheckIfRunning(signal);
  if (!signal_status.ok()) {
    LOG(ERROR) << "Failed to send " << signal_str << " to child process: " << signal_status;
  }

  if (wait_sec > 0) {
    int sleep_ms = 5;
    auto second_wait_until = steady_clock::now() + std::chrono::seconds(wait_sec);
    while (subprocess->IsRunning() && steady_clock::now() < second_wait_until) {
      std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
      // Sleep for 5 ms, 10 ms, 15 ms, etc. capped at 100 ms.
      sleep_ms = std::min(sleep_ms + 5, 100);
    }
  }

  bool is_running = subprocess->IsRunning();
  if (!is_running) {
    LOG(INFO) << "Child process terminated after we sent " << signal_str;
  }
  return !is_running;
}

int main(int argc, char** argv) {
  yb::InitGoogleLoggingSafeBasic(argv[0]);

  vector<string> args;
  args.assign(argv + 1, argv + argc);
  if (args.size() < 2) {
    cerr << "Usage: run-with-timeout <timeout_sec> <program> [<arguments>]" << endl;
    cerr << endl;
    cerr << "Runs the given program with the given timeout. If the program is still" << endl;
    cerr << "running after the given amount of time, it gets sent the SIGQUIT signal" << endl;
    cerr << "to make it create a core dump and terminate. If it is still running after" << endl;
    cerr << "that, we send SIGSEGV, and finally a SIGKILL." << endl;
    cerr << endl;
    cerr << "A timeout value of 0 means no timeout." << endl;
    return EXIT_FAILURE;
  }

  const string& timeout_arg = args[0];
  int timeout_sec;
  try {
    timeout_sec = std::stoi(timeout_arg);
  } catch (const invalid_argument& ex) {
    LOG(ERROR) << "Invalid timeout: " << timeout_arg;
    return EXIT_FAILURE;
  }

  if (timeout_sec < 0) {
    LOG(ERROR) << "Invalid timeout: " << timeout_sec;
    return EXIT_FAILURE;
  }
  const string program = args[1];

#ifdef __linux__
  struct rlimit64 core_limits;
  if (getrlimit64(RLIMIT_CORE, &core_limits) == 0) {
    if (core_limits.rlim_cur != RLIM_INFINITY || core_limits.rlim_max != RLIM_INFINITY) {
      // Only print the debug message if core file limits are not infinite already.
      LOG(INFO) << "Core file size limits set by parent process: "
                << "current=" << core_limits.rlim_cur
                << ", max=" << core_limits.rlim_max << ". Trying to set both to infinity.";
    }
  } else {
    perror("getrlimit64 failed");
  }
  core_limits.rlim_cur = RLIM_INFINITY;
  core_limits.rlim_max = RLIM_INFINITY;
  if (setrlimit64(RLIMIT_CORE, &core_limits) != 0) {
    perror("setrlimit64 failed");
  }
#endif

  {
    auto host_name = GetHostname();
    if (host_name.ok()) {
      LOG(INFO) << "Running on host " << *host_name;
    } else {
      LOG(WARNING) << "Failed to get current host name: " << host_name.status();
    }
  }

  const char* path_env_var = getenv("PATH");
  LOG(INFO) << "PATH environment variable: " << (path_env_var != NULL ? path_env_var : "N/A");

  // Arguments include the program name, so we only erase the first argument from our own args: the
  // timeout value.
  args.erase(args.begin());
  Subprocess subprocess(program, args);
  auto start_time = steady_clock::now();
  if (timeout_sec > 0) {
    LOG(INFO) << "Starting child process with a timeout of " << timeout_sec << " seconds";
  } else {
    LOG(INFO) << "Starting child process with no timeout";
  }
  const Status start_status = subprocess.Start();
  if (!start_status.ok()) {
    LOG(ERROR) << "Failed to start child process: " << start_status.ToString();
    return EXIT_FAILURE;
  }
  LOG(INFO) << "Child process pid: " << subprocess.pid();
  mutex lock;
  condition_variable finished_cond;
  bool finished = false;
  bool timed_out = false;

  thread reaper_thread([timeout_sec, &subprocess, &lock, &finished_cond, &finished, &timed_out] {
    if (timeout_sec == 0) {
      return; // no timeout
    }

    // Wait until the main thread notifies us that the process has finished running, or the
    // timeout is reached.
    auto wait_until_when = steady_clock::now() + std::chrono::seconds(timeout_sec);

    bool finished_local = false;
    {
      unique_lock<mutex> l(lock);
      finished_cond.wait_until(l, wait_until_when, [&finished]{ return finished; });
      finished_local = finished;
    }

    const bool subprocess_is_running = subprocess.IsRunning();
    LOG(INFO) << "Reaper thread: finished=" << finished_local
              << ", subprocess_is_running=" << subprocess_is_running;

    if (!finished_local && subprocess_is_running) {
      timed_out = true;
      if (!SendSignalAndWait(&subprocess, SIGQUIT, "SIGQUIT", kWaitSecAfterSigQuit) &&
          !SendSignalAndWait(&subprocess, SIGSEGV, "SIGSEGV", kWaitSecAfterSigSegv) &&
          !SendSignalAndWait(&subprocess, SIGKILL, "SIGKILL", /* wait_sec */ 0)) {
        LOG(ERROR) << "Failed to kill the process with SIGKILL (should not happen).";
      }
    }
  });

  int waitpid_ret_val = 0;
  CHECK_OK(subprocess.Wait(&waitpid_ret_val));
  LOG(INFO) << "subprocess.Wait finished, waitpid() returned " << waitpid_ret_val;
  {
    unique_lock<mutex> l(lock);
    finished = true;
    finished_cond.notify_one();
  }

  LOG(INFO) << "Waiting for reaper thread to join";
  reaper_thread.join();
  int exit_code = WIFEXITED(waitpid_ret_val) ? WEXITSTATUS(waitpid_ret_val) : 0;
  int term_sig = WIFSIGNALED(waitpid_ret_val) ? WTERMSIG(waitpid_ret_val) : 0;
  LOG(INFO) << "Child process returned exit code " << exit_code;
  if (timed_out) {
    if (exit_code == 0) {
      LOG(ERROR) << "Child process timed out and we had to kill it";
    }
    exit_code = EXIT_FAILURE;
  }

  if (term_sig != 0) {
    if (exit_code == 0) {
      exit_code = EXIT_FAILURE;
    }
    LOG(ERROR) << "Child process terminated due to signal " << term_sig;
  }

  LOG(INFO) << "Returning exit code " << exit_code;
  auto duration_ms = duration_cast<milliseconds>(steady_clock::now() - start_time).count();
  LOG(INFO) << "Total time taken: " << StringPrintf("%.3f", duration_ms / 1000.0) << " sec";
  return exit_code;
}
