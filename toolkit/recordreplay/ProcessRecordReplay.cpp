/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ProcessRecordReplay.h"

#include "ipc/ChildInternal.h"
#include "mozilla/dom/ScriptSettings.h"
#include "mozilla/Compression.h"
#include "mozilla/CycleCollectedJSContext.h"
#include "mozilla/Maybe.h"
#include "mozilla/Sprintf.h"
#include "mozilla/StaticMutex.h"
#include "Lock.h"
#include "ProcessRedirect.h"
#include "ProcessRewind.h"
#include "ValueIndex.h"
#include "pratom.h"
#include "nsPrintfCString.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <mach/exc.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/ndr.h>
#include <sys/time.h>

namespace mozilla {
namespace recordreplay {

MOZ_NEVER_INLINE void BusyWait() {
  static volatile int value = 1;
  while (value) {
  }
}

///////////////////////////////////////////////////////////////////////////////
// Basic interface
///////////////////////////////////////////////////////////////////////////////

Recording* gRecording;

bool gInitialized;
ProcessKind gProcessKind;
const char* gRecordingFilename = "";

// Current process ID.
static int gPid;

// ID of the process which produced the recording.
static int gRecordingPid;

// Whether to spew record/replay messages to stderr.
static bool gSpewEnabled;

// Whether to log extra diagnostic messages.
static bool gVerbose;

// Whether we are replaying on a cloud machine.
static bool gReplayingInCloud;

// Whether we are running an automated test.
static bool gAutomatedTesting;

struct JSFilter {
  std::string mFilename;
  unsigned mStartLine;
  unsigned mEndLine;
};

static void ParseJSFilters(const char* aEnv, InfallibleVector<JSFilter>& aFilters);
static bool FilterMatches(const InfallibleVector<JSFilter>& aFilters,
                          const char* aFilename, unsigned aLine);

// Whether to assert on execution progress changes.
static InfallibleVector<JSFilter> gExecutionAsserts;

// Whether to assert on JS values.
static InfallibleVector<JSFilter> gJSAsserts;

// Firefox installation directory.
static char* gInstallDirectory;

// Location within the installation directory where the current executable
// should be running.
static const char gExecutableSuffix[] =
    "Contents/MacOS/plugin-container.app/Contents/MacOS/plugin-container";

// In a recording/replaying process, time at which execution started per CurrentTime.
static double gStartTime;

static void InitializeCrashDetector();

extern "C" {

MOZ_EXPORT void RecordReplayInterface_Initialize(int aArgc, char* aArgv[]) {
  // Parse command line options for the process kind and recording file.
  Maybe<ProcessKind> processKind;
  Maybe<char*> recordingFile;
  Maybe<char*> replayJSFile;
  for (int i = 0; i < aArgc; i++) {
    if (!strcmp(aArgv[i], gProcessKindOption)) {
      MOZ_RELEASE_ASSERT(processKind.isNothing() && i + 1 < aArgc);
      processKind.emplace((ProcessKind)atoi(aArgv[i + 1]));
    }
    if (!strcmp(aArgv[i], gRecordingFileOption)) {
      MOZ_RELEASE_ASSERT(recordingFile.isNothing() && i + 1 < aArgc);
      recordingFile.emplace(aArgv[i + 1]);
    }
    if (!strcmp(aArgv[i], "-recordReplayJS")) {
      MOZ_RELEASE_ASSERT(replayJSFile.isNothing() && i + 1 < aArgc);
      replayJSFile.emplace(aArgv[i + 1]);
    }
  }
  MOZ_RELEASE_ASSERT(processKind.isSome());

  gProcessKind = processKind.ref();
  if (recordingFile.isSome()) {
    gRecordingFilename = strdup(recordingFile.ref());
  }

  switch (processKind.ref()) {
    case ProcessKind::Recording:
      gIsRecording = gIsRecordingOrReplaying = true;
      fprintf(stderr, "RECORDING %d %s\n", getpid(), gRecordingFilename);
      break;
    case ProcessKind::Replaying:
      gIsReplaying = gIsRecordingOrReplaying = true;
      fprintf(stderr, "REPLAYING %d %s\n", getpid(), gRecordingFilename);
      break;
    case ProcessKind::MiddlemanRecording:
    case ProcessKind::MiddlemanReplaying:
      gIsMiddleman = true;
      fprintf(stderr, "MIDDLEMAN %d %s\n", getpid(), gRecordingFilename);
      break;
    default:
      MOZ_CRASH("Bad ProcessKind");
  }

  if (IsRecording() && TestEnv("MOZ_RECORDING_WAIT_AT_START")) {
    BusyWait();
  }

  if (IsReplaying() && TestEnv("MOZ_REPLAYING_WAIT_AT_START")) {
    BusyWait();
  }

  if (IsMiddleman() && TestEnv("MOZ_MIDDLEMAN_WAIT_AT_START")) {
    BusyWait();
  }

  gPid = getpid();
  if (TestEnv("MOZ_RECORD_REPLAY_SPEW")) {
    gSpewEnabled = true;
  }
  if (TestEnv("RECORD_REPLAY_VERBOSE")) {
    gVerbose = true;
  }

  InitializeRedirections();

  if (!IsRecordingOrReplaying()) {
    InitializeExternalCalls();
    return;
  }

  // Try to determine the install directory from the current executable path.
  const char* executable = aArgv[0];
  size_t executableLength = strlen(executable);
  size_t suffixLength = strlen(gExecutableSuffix);
  if (executableLength >= suffixLength) {
    const char* suffixStart = executable + executableLength - suffixLength;
    if (!strcmp(suffixStart, gExecutableSuffix)) {
      size_t directoryLength = suffixStart - executable;
      gInstallDirectory = new char[directoryLength + 1];
      memcpy(gInstallDirectory, executable, directoryLength);
      gInstallDirectory[directoryLength] = 0;
    }
  }

  InitializeCurrentTime();
  gStartTime = CurrentTime();

  gRecording = new Recording();

  ApplyLibraryRedirections(nullptr);

  Thread::InitializeThreads();

  Thread* thread = Thread::GetById(MainThreadId);
  MOZ_ASSERT(thread->Id() == MainThreadId);

  thread->SetPassThrough(true);

  // The translation layer we are running under in the cloud will intercept this
  // and return a non-zero symbol address.
  gReplayingInCloud = !!dlsym(RTLD_DEFAULT, "RecordReplay_ReplayingInCloud");

  Thread::SpawnAllThreads();
  InitializeExternalCalls();
  if (!gReplayingInCloud) {
    // The crash detector is only useful when we have a local parent process to
    // report crashes to. Avoid initializing it when running in the cloud
    // so that we avoid calling mach interfaces with events passed through.
    InitializeCrashDetector();
  }
  Lock::InitializeLocks();

  // Don't create a stylo thread pool when recording or replaying.
  putenv((char*)"STYLO_THREADS=1");

  child::SetupRecordReplayChannel(aArgc, aArgv);

  if (replayJSFile.isSome()) {
    js::ReadReplayJS(replayJSFile.ref());
  }

  thread->SetPassThrough(false);

  InitializeRewindState();
  gRecordingPid = RecordReplayValue(gPid);
  gAutomatedTesting = TestEnv("RECORD_REPLAY_TEST_SCRIPT");
  ParseJSFilters("RECORD_REPLAY_RECORD_EXECUTION_ASSERTS", gExecutionAsserts);
  ParseJSFilters("RECORD_REPLAY_RECORD_JS_ASSERTS", gJSAsserts);

  gInitialized = true;
}

MOZ_EXPORT size_t
RecordReplayInterface_InternalRecordReplayValue(size_t aValue) {
  Thread* thread = Thread::Current();
  RecordingEventSection res(thread);
  if (!res.CanAccessEvents()) {
    return aValue;
  }

  thread->Events().RecordOrReplayThreadEvent(ThreadEvent::Value);
  thread->Events().RecordOrReplayValue(&aValue);
  return aValue;
}

MOZ_EXPORT void RecordReplayInterface_InternalRecordReplayBytes(void* aData,
                                                                size_t aSize) {
  Thread* thread = Thread::Current();
  RecordingEventSection res(thread);
  if (!res.CanAccessEvents()) {
    return;
  }

  thread->Events().RecordOrReplayThreadEvent(ThreadEvent::Bytes);
  thread->Events().CheckInput(aSize);
  thread->Events().RecordOrReplayBytes(aData, aSize);
}

MOZ_EXPORT void RecordReplayInterface_InternalInvalidateRecording(
    const char* aWhy) {
  if (IsReplaying()) {
    child::ReportFatalError("Recording invalidated while replaying: %s", aWhy);
  }

  Print("INVALIDATE_RECORDING_CRASH\n");
  MOZ_CRASH();
}

MOZ_EXPORT void RecordReplayInterface_InternalBeginPassThroughThreadEventsWithLocalReplay() {
}

MOZ_EXPORT void RecordReplayInterface_InternalEndPassThroughThreadEventsWithLocalReplay() {
}

// The elapsed time since the process was initialized, in seconds. This is
// exposed to the translation layer so that consistent times can be used for
// logged messages.
MOZ_EXPORT double RecordReplayInterface_ElapsedTime() {
  return (CurrentTime() - gStartTime) / 1e6;
}

}  // extern "C"

double ElapsedTime() {
  return RecordReplayInterface_ElapsedTime();
}

// How many bytes have been sent from the recording to the middleman.
size_t gRecordingDataSentToMiddleman;

void FlushRecording(bool aFinishRecording) {
  MOZ_RELEASE_ASSERT(Thread::CurrentIsMainThread());

  if (IsRecording()) {
    gRecording->Flush();
  }

  // Record/replay the size of the recording at this point so that we dispatch
  // the same buffer to SendRecordingData when replaying.
  size_t nbytes = RecordReplayValue(gRecording->Size());

  if (nbytes > gRecordingDataSentToMiddleman) {
    js::SendRecordingData(gRecordingDataSentToMiddleman,
                          gRecording->Data() + gRecordingDataSentToMiddleman,
                          nbytes - gRecordingDataSentToMiddleman,
                          aFinishRecording ? Some(nbytes) : Nothing(),
                          aFinishRecording ? Some(RecordingDuration()) : Nothing());
    gRecordingDataSentToMiddleman = nbytes;
  }
}

void HitEndOfRecording() {
  MOZ_RELEASE_ASSERT(IsReplaying());
  MOZ_RELEASE_ASSERT(!AreThreadEventsPassedThrough());

  if (Thread::CurrentIsMainThread()) {
    // We should have been provided with all the data needed to run forward in
    // the replay. Incorporate any pending data received off thread.
    child::AddPendingRecordingData(/* aRequireMore */ true);
  } else {
    // Non-main threads may wait until more recording data is added.
    Thread::Wait();
  }
}

void AddCheckpointSummary(ProgressCounter aProgress, size_t aElapsed, size_t aTime) {
  MOZ_RELEASE_ASSERT(IsRecording());
  MOZ_RELEASE_ASSERT(Thread::CurrentIsMainThread());

  Stream* stream = gRecording->OpenStream(StreamName::Summary, 0);
  stream->WriteScalar(aProgress);
  stream->WriteScalar(aElapsed);
  stream->WriteScalar(aTime);
}

void GetRecordingSummary(InfallibleVector<ProgressCounter>& aProgressCounters,
                         InfallibleVector<size_t>& aElapsed,
                         InfallibleVector<size_t>& aTimes) {
  MOZ_RELEASE_ASSERT(IsReplaying());
  MOZ_RELEASE_ASSERT(Thread::CurrentIsMainThread());

  Stream* stream = gRecording->OpenStream(StreamName::Summary, 0);
  while (!stream->AtEnd()) {
    aProgressCounters.append(stream->ReadScalar());
    aElapsed.append(stream->ReadScalar());
    aTimes.append(stream->ReadScalar());
  }
}

bool SpewEnabled() { return gSpewEnabled; }

void InternalPrint(const char* aFormat, va_list aArgs) {
  char buf1[2048];
  VsprintfLiteral(buf1, aFormat, aArgs);
  char buf2[2048];
  SprintfLiteral(buf2, "Spew[%d]: %s", gPid, buf1);
  DirectPrint(buf2);
}

const char* ThreadEventName(ThreadEvent aEvent) {
  switch (aEvent) {
#define EnumToString(Kind) \
  case ThreadEvent::Kind:  \
    return #Kind;
    ForEachThreadEvent(EnumToString)
#undef EnumToString
        case ThreadEvent::CallStart : break;
  }
  size_t callId = (size_t)aEvent - (size_t)ThreadEvent::CallStart;
  return GetRedirection(callId).mName;
}

int GetRecordingPid() { return gRecordingPid; }
int GetPid() { return gPid; }

void ResetPid() { gPid = getpid(); }

bool ReplayingInCloud() { return gReplayingInCloud; }
const char* InstallDirectory() { return gInstallDirectory; }

bool IsVerbose() { return gVerbose; }

///////////////////////////////////////////////////////////////////////////////
// Record/Replay Assertions
///////////////////////////////////////////////////////////////////////////////

extern "C" {

MOZ_EXPORT void RecordReplayInterface_InternalRecordReplayAssert(
    const char* aFormat, va_list aArgs) {
  Thread* thread = Thread::Current();
  RecordingEventSection res(thread);
  if (!res.CanAccessEvents()) {
    return;
  }

  // Add the asserted string to the recording.
  nsAutoCString text;
  text.AppendPrintf(aFormat, aArgs);

  // This must be kept in sync with Stream::RecordOrReplayThreadEvent, which
  // peeks at the input string written after the thread event.
  thread->Events().RecordOrReplayThreadEvent(ThreadEvent::Assert, text.get());
  thread->Events().CheckInput(text.get());
}

MOZ_EXPORT void RecordReplayInterface_InternalRecordReplayAssertBytes(
    const void* aData, size_t aSize) {
  Thread* thread = Thread::Current();
  RecordingEventSection res(thread);
  if (!res.CanAccessEvents()) {
    return;
  }

  thread->Events().RecordOrReplayThreadEvent(ThreadEvent::AssertBytes);
  thread->Events().CheckInput(aData, aSize);
}

MOZ_EXPORT void RecordReplayAssertFromC(const char* aText) {
  RecordReplayAssert(aText);
}

}  // extern "C"

static ValueIndex* gGenericThings;
static StaticMutexNotRecorded gGenericThingsMutex;

// JS event that advanced the execution progress counter.
struct RecentJSEvent {
  ProgressCounter mProgress = 0;
  std::string mFilename;
  unsigned mLineno = 0;
  unsigned mColumn = 0;
};

static StaticInfallibleVector<RecentJSEvent> gRecentJSEvents;
static const size_t NumRecentJSEvents = 10000;

size_t gRecentJSEventsIndex = 0;

static void AdvanceRecentJSEventsIndex() {
  gRecentJSEventsIndex = (gRecentJSEventsIndex + 1) % gRecentJSEvents.length();
}

static void AddRecentJS(const char* aFilename, unsigned aLineno, unsigned aColumn) {
  if (gRecentJSEvents.length() == 0) {
    gRecentJSEvents.appendN(RecentJSEvent(), NumRecentJSEvents);
  }

  RecentJSEvent& event = gRecentJSEvents[gRecentJSEventsIndex];
  event.mProgress = *ExecutionProgressCounter();
  event.mFilename = aFilename;
  event.mLineno = aLineno;
  event.mColumn = aColumn;
  AdvanceRecentJSEventsIndex();
}

void DumpRecentJS(FileHandle aFd) {
  size_t limit = gRecentJSEventsIndex;
  AdvanceRecentJSEventsIndex();
  while (gRecentJSEventsIndex != limit) {
    const RecentJSEvent& event = gRecentJSEvents[gRecentJSEventsIndex];
    if (event.mFilename.length()) {
      nsPrintfCString text("JS Progress %llu: %s:%u:%u\n",
                           event.mProgress, event.mFilename.c_str(),
                           event.mLineno, event.mColumn);
      DirectWriteString(aFd, text.get());
    }
    AdvanceRecentJSEventsIndex();
  }
}

extern "C" {

MOZ_EXPORT void RecordReplayInterface_InternalRegisterThing(void* aThing) {
  if (AreThreadEventsPassedThrough()) {
    return;
  }

  AutoOrderedAtomicAccess at(&gGenericThings);
  StaticMutexAutoLock lock(gGenericThingsMutex);
  if (!gGenericThings) {
    gGenericThings = new ValueIndex();
  }
  if (gGenericThings->Contains(aThing)) {
    gGenericThings->Remove(aThing);
  }
  gGenericThings->Insert(aThing);
}

MOZ_EXPORT void RecordReplayInterface_InternalUnregisterThing(void* aThing) {
  StaticMutexAutoLock lock(gGenericThingsMutex);
  if (gGenericThings) {
    gGenericThings->Remove(aThing);
  }
}

MOZ_EXPORT size_t RecordReplayInterface_InternalThingIndex(void* aThing) {
  if (!aThing) {
    return 0;
  }
  StaticMutexAutoLock lock(gGenericThingsMutex);
  size_t index = 0;
  if (gGenericThings) {
    gGenericThings->MaybeGetIndex(aThing, &index);
  }
  return index;
}

MOZ_EXPORT const char* RecordReplayInterface_InternalVirtualThingName(
    void* aThing) {
  void* vtable = *(void**)aThing;
  nsAutoCString value;
  SymbolNameRaw(vtable, value);

  // Leaks... only use this for debugging.
  char* rv = new char[value.Length() + 1];
  strcpy(rv, value.get());
  return rv;
}

MOZ_EXPORT void RecordReplayInterface_InternalHoldJSObject(void* aJSObj) {
  if (aJSObj) {
    JSContext* cx = dom::danger::GetJSContext();
    JS::PersistentRootedObject* root = new JS::PersistentRootedObject(cx);
    *root = static_cast<JSObject*>(aJSObj);
  }
}

MOZ_EXPORT bool RecordReplayInterface_LoadedWithFileURI() {
  return RecordReplayValue(!strncmp(gRecordingFilename, "file://", 7));
}

MOZ_EXPORT bool RecordReplayInterface_InternalInAutomatedTest() {
  return gAutomatedTesting;
}

MOZ_EXPORT void RecordReplayInterface_InternalAssertScriptedCaller(const char* aWhy) {
  JS::AutoFilename filename;
  unsigned lineno;
  unsigned column;
  JSContext* cx = nullptr;
  if (NS_IsMainThread() && CycleCollectedJSContext::Get()) {
    cx = dom::danger::GetJSContext();
  }
  if (cx && JS::DescribeScriptedCaller(cx, &filename, &lineno, &column)) {
    RecordReplayAssert("%s %s:%u:%u", aWhy, filename.get(), lineno, column);
  } else {
    RecordReplayAssert("%s NoScriptedCaller", aWhy);
  }
}

MOZ_EXPORT void RecordReplayInterface_ExecutionProgressHook(const char* aFilename, unsigned aLineno,
                                                            unsigned aColumn) {
  Thread* thread = Thread::Current();
  MOZ_RELEASE_ASSERT(thread->IsMainThread());

  if (!thread->HasDivergedFromRecording()) {
    MOZ_RELEASE_ASSERT(!thread->AreEventsDisallowed());
    MOZ_RELEASE_ASSERT(!thread->PassThroughEvents());

    if (FilterMatches(gExecutionAsserts, aFilename, aLineno)) {
      RecordReplayAssert("ExecutionProgress %s:%u:%u", aFilename, aLineno, aColumn);
    }

    AddRecentJS(aFilename, aLineno, aColumn);
  }
}

MOZ_EXPORT bool RecordReplayInterface_ShouldEmitRecordReplayAssert(const char* aFilename,
                                                                   unsigned aLineno,
                                                                   unsigned aColumn) {
  return FilterMatches(gJSAsserts, aFilename, aLineno);
}

}  // extern "C"

static void ParseJSFilters(const char* aEnv, InfallibleVector<JSFilter>& aFilters) {
  const char* value = getenv(aEnv);
  if (!value) {
    return;
  }

  while (true) {
    JSFilter filter;

    const char* end = strchr(value, '@');
    if (!end) {
      break;
    }

    filter.mFilename = std::string(value, end - value);
    value = end + 1;

    end = strchr(value, '@');
    if (!end) {
      break;
    }

    filter.mStartLine = atoi(value);
    value = end + 1;

    filter.mEndLine = atoi(value);

    Print("ParseJSFilter %s %s %u %u\n", aEnv,
          filter.mFilename.c_str(), filter.mStartLine, filter.mEndLine);
    aFilters.append(filter);

    end = strchr(value, '@');
    if (!end) {
      break;
    }

    value = end + 1;
  }
}

static bool FilterMatches(const InfallibleVector<JSFilter>& aFilters,
                          const char* aFilename, unsigned aLine) {
  for (const JSFilter& filter : aFilters) {
    if (strstr(aFilename, filter.mFilename.c_str()) &&
        aLine >= filter.mStartLine &&
        aLine <= filter.mEndLine) {
      return true;
    }
  }
  return false;
}

static mach_port_t gCrashDetectorExceptionPort;

// See AsmJSSignalHandlers.cpp.
static const mach_msg_id_t sExceptionId = 2405;

// This definition was generated by mig (the Mach Interface Generator) for the
// routine 'exception_raise' (exc.defs). See js/src/wasm/WasmSignalHandlers.cpp.
#pragma pack(4)
typedef struct {
  mach_msg_header_t Head;
  /* start of the kernel processed data */
  mach_msg_body_t msgh_body;
  mach_msg_port_descriptor_t thread;
  mach_msg_port_descriptor_t task;
  /* end of the kernel processed data */
  NDR_record_t NDR;
  exception_type_t exception;
  mach_msg_type_number_t codeCnt;
  int64_t code[2];
} Request__mach_exception_raise_t;
#pragma pack()

typedef struct {
  Request__mach_exception_raise_t body;
  mach_msg_trailer_t trailer;
} ExceptionRequest;

static void CrashDetectorThread(void*) {
  kern_return_t kret;

  while (true) {
    ExceptionRequest request;
    kret = mach_msg(&request.body.Head, MACH_RCV_MSG, 0, sizeof(request),
                    gCrashDetectorExceptionPort, MACH_MSG_TIMEOUT_NONE,
                    MACH_PORT_NULL);
    Print("Crashing: %s\n", gMozCrashReason);

    kern_return_t replyCode = KERN_FAILURE;
    if (kret == KERN_SUCCESS && request.body.Head.msgh_id == sExceptionId &&
        request.body.exception == EXC_BAD_ACCESS && request.body.codeCnt == 2) {
      uint8_t* faultingAddress = (uint8_t*)request.body.code[1];
      child::MinidumpInfo info(request.body.exception, request.body.code[0],
                               request.body.code[1],
                               request.body.thread.name,
                               request.body.task.name);
      child::ReportCrash(info, faultingAddress);
    } else {
      child::ReportFatalError("CrashDetectorThread mach_msg "
                              "returned unexpected data");
    }

    __Reply__exception_raise_t reply;
    reply.Head.msgh_bits =
        MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(request.body.Head.msgh_bits), 0);
    reply.Head.msgh_size = sizeof(reply);
    reply.Head.msgh_remote_port = request.body.Head.msgh_remote_port;
    reply.Head.msgh_local_port = MACH_PORT_NULL;
    reply.Head.msgh_id = request.body.Head.msgh_id + 100;
    reply.NDR = NDR_record;
    reply.RetCode = replyCode;
    mach_msg(&reply.Head, MACH_SEND_MSG, sizeof(reply), 0, MACH_PORT_NULL,
             MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
  }
}

static void InitializeCrashDetector() {
  MOZ_RELEASE_ASSERT(AreThreadEventsPassedThrough());
  kern_return_t kret;

  // Get a port which can send and receive data.
  kret = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                            &gCrashDetectorExceptionPort);
  MOZ_RELEASE_ASSERT(kret == KERN_SUCCESS);

  kret = mach_port_insert_right(mach_task_self(), gCrashDetectorExceptionPort,
                                gCrashDetectorExceptionPort,
                                MACH_MSG_TYPE_MAKE_SEND);
  MOZ_RELEASE_ASSERT(kret == KERN_SUCCESS);

  // Create a thread to block on reading the port.
  Thread::SpawnNonRecordedThread(CrashDetectorThread, nullptr);

  // Set exception ports on the entire task. Unfortunately, this clobbers any
  // other exception ports for the task, and forwarding to those other ports
  // is not easy to get right.
  kret = task_set_exception_ports(
      mach_task_self(), EXC_MASK_BAD_ACCESS, gCrashDetectorExceptionPort,
      EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES, THREAD_STATE_NONE);
  MOZ_RELEASE_ASSERT(kret == KERN_SUCCESS);
}

}  // namespace recordreplay
}  // namespace mozilla
