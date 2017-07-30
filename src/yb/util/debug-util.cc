// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "yb/util/debug-util.h"

#include <execinfo.h>
#include <dirent.h>
#include <signal.h>
#include <sys/syscall.h>

#ifdef __linux__
#include <link.h>
#include <backtrace.h>
#include <cxxabi.h>
#endif  // __linux__

#include <string>
#include <iostream>

#include <glog/logging.h>

#include "yb/gutil/macros.h"
#include "yb/gutil/singleton.h"
#include "yb/gutil/spinlock.h"
#include "yb/gutil/stringprintf.h"
#include "yb/gutil/strings/numbers.h"
#include "yb/util/env.h"
#include "yb/util/errno.h"
#include "yb/util/memory/memory.h"
#include "yb/util/monotime.h"
#include "yb/util/thread.h"

#if defined(__APPLE__)
typedef sig_t sighandler_t;
#endif

// Evil hack to grab a few useful functions from glog
namespace google {

extern int GetStackTrace(void** result, int max_depth, int skip_count);

// Symbolizes a program counter.  On success, returns true and write the
// symbol name to "out".  The symbol name is demangled if possible
// (supports symbols generated by GCC 3.x or newer).  Otherwise,
// returns false.
bool Symbolize(void *pc, char *out, int out_size);

namespace glog_internal_namespace_ {
extern void DumpStackTraceToString(std::string *s);
} // namespace glog_internal_namespace_
} // namespace google

// The %p field width for printf() functions is two characters per byte.
// For some environments, add two extra bytes for the leading "0x".
static const int kPrintfPointerFieldWidth = 2 + 2 * sizeof(void*);

// The signal that we'll use to communicate with our other threads.
// This can't be in used by other libraries in the process.
static int g_stack_trace_signum = SIGUSR2;

// We only allow a single dumper thread to run at a time. This simplifies the synchronization
// between the dumper and the target thread.
//
// This lock also protects changes to the signal handler.
static base::SpinLock g_dumper_thread_lock(base::LINKER_INITIALIZED);

using std::string;

namespace yb {

namespace {

// Global structure used to communicate between the signal handler
// and a dumping thread.
struct SignalCommunication {
  // The actual stack trace collected from the target thread.
  StackTrace stack;

  // The current target. Signals can be delivered asynchronously, so the
  // dumper thread sets this variable first before sending a signal. If
  // a signal is received on a thread that doesn't match 'target_tid', it is
  // ignored.
  pid_t target_tid;

  // Set to 1 when the target thread has successfully collected its stack.
  // The dumper thread spins waiting for this to become true.
  Atomic32 result_ready;

  // Lock protecting the other members. We use a bare atomic here and a custom
  // lock guard below instead of existing spinlock implementaitons because futex()
  // is not signal-safe.
  Atomic32 lock;

  struct Lock;
};
SignalCommunication g_comm;

// Pared-down SpinLock for SignalCommunication::lock. This doesn't rely on futex
// so it is async-signal safe.
struct SignalCommunication::Lock {
  Lock() {
    while (base::subtle::Acquire_CompareAndSwap(&g_comm.lock, 0, 1) != 0) {
      sched_yield();
    }
  }
  ~Lock() {
    base::subtle::Release_Store(&g_comm.lock, 0);
  }
};

// Signal handler for our stack trace signal.
// We expect that the signal is only sent from DumpThreadStack() -- not by a user.
void HandleStackTraceSignal(int signum) {
  int old_errno = errno;
  SignalCommunication::Lock l;

  // Check that the dumper thread is still interested in our stack trace.
  // It's possible for signal delivery to be artificially delayed, in which
  // case the dumper thread would have already timed out and moved on with
  // its life. In that case, we don't want to race with some other thread's
  // dump.
  int64_t my_tid = Thread::CurrentThreadId();
  if (g_comm.target_tid != my_tid) {
    errno = old_errno;
    return;
  }

  g_comm.stack.Collect(2);
  base::subtle::Release_Store(&g_comm.result_ready, 1);
  errno = old_errno;
}

bool InitSignalHandlerUnlocked(int signum) {
  enum InitState {
    UNINITIALIZED,
    INIT_ERROR,
    INITIALIZED
  };
  static InitState state = UNINITIALIZED;

  // If we've already registered a handler, but we're being asked to
  // change our signal, unregister the old one.
  if (signum != g_stack_trace_signum && state == INITIALIZED) {
    struct sigaction old_act;
    PCHECK(sigaction(g_stack_trace_signum, nullptr, &old_act) == 0);
    if (old_act.sa_handler == &HandleStackTraceSignal) {
      signal(g_stack_trace_signum, SIG_DFL);
    }
  }

  // If we'd previously had an error, but the signal number
  // is changing, we should mark ourselves uninitialized.
  if (signum != g_stack_trace_signum) {
    g_stack_trace_signum = signum;
    state = UNINITIALIZED;
  }

  if (state == UNINITIALIZED) {
    struct sigaction old_act;
    PCHECK(sigaction(g_stack_trace_signum, nullptr, &old_act) == 0);
    if (old_act.sa_handler != SIG_DFL &&
        old_act.sa_handler != SIG_IGN) {
      state = INIT_ERROR;
      LOG(WARNING) << "signal handler for stack trace signal "
                   << g_stack_trace_signum
                   << " is already in use: "
                   << "YB will not produce thread stack traces.";
    } else {
      // No one appears to be using the signal. This is racy, but there is no
      // atomic swap capability.
      sighandler_t old_handler = signal(g_stack_trace_signum, HandleStackTraceSignal);
      if (old_handler != SIG_IGN &&
          old_handler != SIG_DFL) {
        LOG(FATAL) << "raced against another thread installing a signal handler";
      }
      state = INITIALIZED;
    }
  }
  return state == INITIALIZED;
}

const char kStackTraceEntryFormat[] = "    @ %*p  %s";
const char kUnknownSymbol[] = "(unknown)";

#ifdef __linux__

// Remove path prefixes up to what looks like the root of the YB source tree.
const char* NormalizeSourceFilePath(const char* file_path) {
  if (file_path == nullptr) {
    return file_path;
  }
  const char* const src_yb_subpath = strstr(file_path, "/src/yb/");
  if (src_yb_subpath != nullptr) {
    return src_yb_subpath + 5;
  }
  const char* const src_rocksdb_subpath = strstr(file_path, "/src/rocksdb/");
  if (src_rocksdb_subpath != nullptr) {
    return src_rocksdb_subpath + 5;
  }
  const char* const thirdparty_subpath = strstr(file_path, "/thirdparty/");
  if (thirdparty_subpath != nullptr) {
    return thirdparty_subpath + 1;
  }
  return file_path;
}

void BacktraceErrorCallback(void* data, const char* msg, int errnum) {
  string* const buf = reinterpret_cast<string*>(data);
  buf->append(StringPrintf("Backtrace error: %s (errnum=%d)\n", msg, errnum));
}

class GlobalBacktraceState {
 public:
  GlobalBacktraceState() {
    bt_state_ = backtrace_create_state(
        nullptr, /* threaded = */ 1, BacktraceErrorCallback, nullptr);
  }

  backtrace_state* GetState() { return bt_state_; }
 private:
  struct backtrace_state* bt_state_;
};

struct SymbolizationContext {
  StackTraceLineFormat stack_trace_line_format = StackTraceLineFormat::DEFAULT;
  string* buf = nullptr;
};

int BacktraceFullCallback(void *const data, const uintptr_t pc,
                          const char* const filename, const int lineno,
                          const char* const original_function_name) {
  assert(data != nullptr);
  const SymbolizationContext& context = *pointer_cast<SymbolizationContext*>(data);
  string* const buf = context.buf;
  int demangle_status = 0;
  char* const demangled_function_name =
      original_function_name != nullptr ?
      abi::__cxa_demangle(original_function_name, 0, 0, &demangle_status) :
      nullptr;
  const char* function_name_to_use = original_function_name;
  if (original_function_name != nullptr) {
    if (demangle_status != 0) {
      if (demangle_status != -2) {
        // -2 means the mangled name is not a valid name under the C++ ABI mangling rules.
        // This happens when the name is e.g. "main", so we don't report the error.
        StringAppendF(buf, "Error: __cxa_demangle failed for '%s' with error code %d\n",
            original_function_name, demangle_status);
      }
      // Regardless of the exact reason for demangle failure, we use the original function name
      // provided by libbacktrace.
    } else if (demangled_function_name != nullptr) {
      // If __cxa_demangle returns 0 and a non-null string, we use that instead of the original
      // function name.
      function_name_to_use = demangled_function_name;
    } else {
      StringAppendF(buf,
          "Error: __cxa_demangle returned zero status but nullptr demangled function for '%s'\n",
          original_function_name);
    }
  }

  if (function_name_to_use == nullptr) {
    function_name_to_use = kUnknownSymbol;
  }

  const string frame_without_file_line =
      StringPrintf(kStackTraceEntryFormat, kPrintfPointerFieldWidth,
          reinterpret_cast<void*>(pc), function_name_to_use);
  // We have not appended an end-of-line character yet. Let's see if we have file name / line number
  // information first. BTW kStackTraceEntryFormat is used both here and in glog-based
  // symbolization.
  if (filename != nullptr) {
    // Got filename and line number from libbacktrace! No need to filter the output through
    // addr2line, etc.
    if (context.stack_trace_line_format == StackTraceLineFormat::CLION_CLICKABLE) {
      const string file_line_prefix = StringPrintf("%s:%d: ", filename, lineno);
      StringAppendF(buf, "%-100s", file_line_prefix.c_str());
      *buf += frame_without_file_line;
    } else {
      // Must be StackTraceLineFormat::SHORT.
      *buf += frame_without_file_line;
      StringAppendF(buf, " (%s:%d)", NormalizeSourceFilePath(filename), lineno);
    }
  }
  buf->push_back('\n');
  // No need to check for nullptr, free is a no-op in that case.
  free(demangled_function_name);
  return 0;
}

#endif  // __linux__

}  // anonymous namespace

Status SetStackTraceSignal(int signum) {
  base::SpinLockHolder h(&g_dumper_thread_lock);
  if (!InitSignalHandlerUnlocked(signum)) {
    return STATUS(InvalidArgument, "unable to install signal handler");
  }
  return Status::OK();
}

std::string DumpThreadStack(int64_t tid) {
#if defined(__linux__)
  base::SpinLockHolder h(&g_dumper_thread_lock);

  // Ensure that our signal handler is installed. We don't need any fancy GoogleOnce here
  // because of the mutex above.
  if (!InitSignalHandlerUnlocked(g_stack_trace_signum)) {
    return "<unable to take thread stack: signal handler unavailable>";
  }

  // Set the target TID in our communication structure, so if we end up with any
  // delayed signal reaching some other thread, it will know to ignore it.
  {
    SignalCommunication::Lock l;
    CHECK_EQ(0, g_comm.target_tid);
    g_comm.target_tid = tid;
  }

  // We use the raw syscall here instead of kill() to ensure that we don't accidentally
  // send a signal to some other process in the case that the thread has exited and
  // the TID been recycled.
  if (syscall(SYS_tgkill, getpid(), tid, g_stack_trace_signum) != 0) {
    {
      SignalCommunication::Lock l;
      g_comm.target_tid = 0;
    }
    return "(unable to deliver signal: process may have exited)";
  }

  // We give the thread ~1s to respond. In testing, threads typically respond within
  // a few iterations of the loop, so this timeout is very conservative.
  //
  // The main reason that a thread would not respond is that it has blocked signals. For
  // example, glibc's timer_thread doesn't respond to our signal, so we always time out
  // on that one.
  string buf;
  int i = 0;
  while (!base::subtle::Acquire_Load(&g_comm.result_ready) &&
         i++ < 100) {
    SleepFor(MonoDelta::FromMilliseconds(10));
  }

  {
    SignalCommunication::Lock l;
    CHECK_EQ(tid, g_comm.target_tid);

    if (!g_comm.result_ready) {
      buf = "(thread did not respond: maybe it is blocking signals)";
    } else {
      buf = g_comm.stack.Symbolize();
    }

    g_comm.target_tid = 0;
    g_comm.result_ready = 0;
  }
  return buf;
#else // defined(__linux__)
  return "(unsupported platform)";
#endif
}

Status ListThreads(vector<pid_t> *tids) {
#if defined(__linux__)
  DIR *dir = opendir("/proc/self/task/");
  if (dir == NULL) {
    return STATUS(IOError, "failed to open task dir", ErrnoToString(errno), errno);
  }
  struct dirent *d;
  while ((d = readdir(dir)) != NULL) {
    if (d->d_name[0] != '.') {
      uint32_t tid;
      if (!safe_strtou32(d->d_name, &tid)) {
        LOG(WARNING) << "bad tid found in procfs: " << d->d_name;
        continue;
      }
      tids->push_back(tid);
    }
  }
  closedir(dir);
#endif // defined(__linux__)
  return Status::OK();
}

std::string GetStackTrace(StackTraceLineFormat stack_trace_line_format,
                          int num_top_frames_to_skip) {
  std::string buf;
#ifdef __linux__
  SymbolizationContext context;
  context.buf = &buf;
  context.stack_trace_line_format = stack_trace_line_format;
  // Use libbacktrace on Linux because that gives us file names and line numbers.
  struct backtrace_state* const backtrace_state =
      Singleton<GlobalBacktraceState>::get()->GetState();
  const int backtrace_full_rv = backtrace_full(
      backtrace_state, /* skip = */ num_top_frames_to_skip + 1, BacktraceFullCallback,
      BacktraceErrorCallback, &context);
  if (backtrace_full_rv != 0) {
    StringAppendF(&buf, "Error: backtrace_full return value is %d", backtrace_full_rv);
  }
#else
  google::glog_internal_namespace_::DumpStackTraceToString(&buf);
#endif
  return buf;
}

std::string GetStackTraceHex() {
  char buf[1024];
  HexStackTraceToString(buf, 1024);
  return std::string(buf);
}

void HexStackTraceToString(char* buf, size_t size) {
  StackTrace trace;
  trace.Collect(1);
  trace.StringifyToHex(buf, size);
}

string GetLogFormatStackTraceHex() {
  StackTrace trace;
  trace.Collect(1);
  return trace.ToLogFormatHexString();
}

void StackTrace::Collect(int skip_frames) {
  num_frames_ = google::GetStackTrace(frames_, arraysize(frames_), skip_frames);
}

void StackTrace::StringifyToHex(char* buf, size_t size, int flags) const {
  char* dst = buf;

  // Reserve kHexEntryLength for the first iteration of the loop, 1 byte for a
  // space (which we may not need if there's just one frame), and 1 for a nul
  // terminator.
  char* limit = dst + size - kHexEntryLength - 2;
  for (int i = 0; i < num_frames_ && dst < limit; i++) {
    if (i != 0) {
      *dst++ = ' ';
    }
    // See note in Symbolize() below about why we subtract 1 from each address here.
    uintptr_t addr = reinterpret_cast<uintptr_t>(frames_[i]);
    if (!(flags & NO_FIX_CALLER_ADDRESSES)) {
      addr--;
    }
    FastHex64ToBuffer(addr, dst);
    dst += kHexEntryLength;
  }
  *dst = '\0';
}

string StackTrace::ToHexString(int flags) const {
  // Each frame requires kHexEntryLength, plus a space
  // We also need one more byte at the end for '\0'
  char buf[kMaxFrames * (kHexEntryLength + 1) + 1];
  StringifyToHex(buf, arraysize(buf), flags);
  return string(buf);
}

// Symbolization function borrowed from glog and modified to use libbacktrace on Linux.
string StackTrace::Symbolize(const StackTraceLineFormat stack_trace_line_format) const {
  string buf;
#ifdef __linux__
  // Use libbacktrace for symbolization.
  struct backtrace_state* const backtrace_state =
      Singleton<GlobalBacktraceState>::get()->GetState();
#endif

  for (int i = 0; i < num_frames_; i++) {
    void* const pc = frames_[i];

    // The return address 'pc' on the stack is the address of the instruction
    // following the 'call' instruction. In the case of calling a function annotated
    // 'noreturn', this address may actually be the first instruction of the next
    // function, because the function we care about ends with the 'call'.
    // So, we subtract 1 from 'pc' so that we're pointing at the 'call' instead
    // of the return address.
    //
    // For example, compiling a C program with -O2 that simply calls 'abort()' yields
    // the following disassembly:
    //     Disassembly of section .text:
    //
    //     0000000000400440 <main>:
    //       400440:   48 83 ec 08             sub    $0x8,%rsp
    //       400444:   e8 c7 ff ff ff          callq  400410 <abort@plt>
    //
    //     0000000000400449 <_start>:
    //       400449:   31 ed                   xor    %ebp,%ebp
    //       ...
    //
    // If we were to take a stack trace while inside 'abort', the return pointer
    // on the stack would be 0x400449 (the first instruction of '_start'). By subtracting
    // 1, we end up with 0x400448, which is still within 'main'.
    //
    // This also ensures that we point at the correct line number when using addr2line
    // on logged stacks.
    void* const adjusted_pc = reinterpret_cast<char *>(pc) - 1;

#ifdef __linux__
    SymbolizationContext context;
    context.stack_trace_line_format = stack_trace_line_format;
    context.buf = &buf;
    backtrace_pcinfo(backtrace_state, reinterpret_cast<uintptr_t>(adjusted_pc),
        BacktraceFullCallback, BacktraceErrorCallback, &context);
#else
    char tmp[1024];
    const char* symbol = kUnknownSymbol;

    if (google::Symbolize(adjusted_pc, tmp, sizeof(tmp))) {
      symbol = tmp;
    }
    StringAppendF(&buf, kStackTraceEntryFormat, kPrintfPointerFieldWidth, adjusted_pc, symbol);
    // We are appending the end-of-line character separately because we want to reuse the same
    // format string for libbacktrace callback and glog-based symbolization, and we have an extra
    // file name / line number component before the end-of-line in the libbacktrace case.
    buf.push_back('\n');
#endif  // Non-linux implementation
  }

  // TODO: what do we need to do to free the backtrace state?
  return buf;
}

string StackTrace::ToLogFormatHexString() const {
  string buf;
  for (int i = 0; i < num_frames_; i++) {
    void* pc = frames_[i];
    StringAppendF(&buf, "    @ %*p\n", kPrintfPointerFieldWidth, pc);
  }
  return buf;
}

uint64_t StackTrace::HashCode() const {
  return util_hash::CityHash64(reinterpret_cast<const char*>(frames_),
                               sizeof(frames_[0]) * num_frames_);
}

namespace {
#ifdef __linux__
int DynamcLibraryListCallback(struct dl_phdr_info *info, size_t size, void *data) {
  if (*info->dlpi_name != '\0') {
    // We can't use LOG(...) yet because Google Logging might not be initialized.
    // It is also important to write the entire line at once so that it is less likely to be
    // interleaved with pieces of similar lines from other processes.
    std::cerr << StringPrintf(
        "Shared library '%s' loaded at address 0x%" PRIx64 "\n", info->dlpi_name, info->dlpi_addr);
  }
  return 0;
}
#endif

void PrintLoadedDynamicLibraries() {
#ifdef __linux__
  // Supported on Linux only.
  dl_iterate_phdr(DynamcLibraryListCallback, nullptr);
#endif
}

bool PrintLoadedDynamicLibrariesOnceHelper() {
  const char* list_dl_env_var = std::getenv("YB_LIST_LOADED_DYNAMIC_LIBS");
  if (list_dl_env_var != nullptr && *list_dl_env_var != '\0') {
    PrintLoadedDynamicLibraries();
  }
  return true;
}
}  // anonymous namespace

// List the load addresses of dynamic libraries once on process startup if required.
const bool  __attribute__((unused)) kPrintedLoadedDynamicLibraries =
    PrintLoadedDynamicLibrariesOnceHelper();

}  // namespace yb
