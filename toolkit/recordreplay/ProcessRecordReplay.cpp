/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ProcessRecordReplay.h"

#include "JSControl.h"
#include "mozilla/BasicEvents.h"
#include "mozilla/dom/BrowserChild.h"
#include "mozilla/dom/ScriptSettings.h"
#include "mozilla/Compression.h"
#include "mozilla/CycleCollectedJSContext.h"
#include "mozilla/Maybe.h"
#include "mozilla/Sprintf.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/VsyncDispatcher.h"
#include "nsAppRunner.h"
#include "pratom.h"
#include "nsPrintfCString.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/time.h>

#ifdef XP_MACOSX
#include "mozilla/MacLaunchHelper.h"
#endif

extern "C" void RecordReplayOrderDefaultTimeZoneMutex();

namespace mozilla {

namespace image {
  extern void RecordReplayInitializeSurfaceCacheMutex();
}

namespace recordreplay {

MOZ_NEVER_INLINE void BusyWait() {
  static volatile int value = 1;
  while (value) {
  }
}

///////////////////////////////////////////////////////////////////////////////
// Basic interface
///////////////////////////////////////////////////////////////////////////////

struct JSFilter {
  std::string mFilename;
  unsigned mStartLine = 0;
  unsigned mEndLine = 0;
};

static void ParseJSFilters(const char* aEnv, InfallibleVector<JSFilter>& aFilters);
static bool FilterMatches(const InfallibleVector<JSFilter>& aFilters,
                          const char* aFilename, unsigned aLine);

// Whether to assert on execution progress changes.
static InfallibleVector<JSFilter> gExecutionAsserts;

// Whether to assert on JS values.
static InfallibleVector<JSFilter> gJSAsserts;

static void (*gAttach)(const char* dispatch, const char* buildId);
static void (*gSetApiKey)(const char* apiKey);
static void (*gRecordCommandLineArguments)(int*, char***);
static uintptr_t (*gRecordReplayValue)(const char* why, uintptr_t value);
static void (*gRecordReplayBytes)(const char* why, void* buf, size_t size);
static void (*gPrintVA)(const char* format, va_list args);
static void (*gRegisterPointer)(void* ptr);
static void (*gUnregisterPointer)(void* ptr);
static int (*gPointerId)(void* ptr);
static void (*gAssert)(const char* format, va_list);
static void (*gAssertBytes)(const char* why, const void*, size_t);
static void (*gSaveRecording)(const char* dir);
static void (*gFinishRecording)();
static uint64_t* (*gProgressCounter)();
static void (*gSetProgressCallback)(void (*aCallback)(uint64_t));
static void (*gProgressReached)();
static void (*gBeginPassThroughEvents)();
static void (*gEndPassThroughEvents)();
static bool (*gAreEventsPassedThrough)();
static void (*gBeginDisallowEvents)();
static void (*gEndDisallowEvents)();
static bool (*gAreEventsDisallowed)();
static bool (*gHasDivergedFromRecording)();
static bool (*gIsUnhandledDivergenceAllowed)();
static void (*gRecordReplayNewCheckpoint)();
static bool (*gRecordReplayIsReplaying)();
static int (*gCreateOrderedLock)(const char* aName);
static void (*gOrderedLock)(int aLock);
static void (*gOrderedUnlock)(int aLock);
static void (*gAddOrderedPthreadMutex)(const char* aName, pthread_mutex_t* aMutex);
static void (*gOnMouseEvent)(const char* aKind, size_t aClientX, size_t aClientY);
static void (*gOnKeyEvent)(const char* aKind, const char* aKey);
static void (*gSetRecordingIdCallback)(void (*aCallback)(const char*));
static void (*gProcessRecording)();
static void (*gSetCrashReasonCallback)(const char* (*aCallback)());
static void (*gInvalidateRecording)(const char* aFormat, ...);
static void (*gSetCrashNote)(const char* aNote);

static void* gDriverHandle;

void LoadSymbolInternal(const char* name, void** psym, bool aOptional) {
  *psym = dlsym(gDriverHandle, name);
  if (!*psym && !aOptional) {
    fprintf(stderr, "Could not find %s in Record Replay driver, crashing.\n", name);
    MOZ_CRASH();
  }
}

static void RecordingIdCallback(const char* aRecordingId);

// This is called when the process crashes to return any reason why Gecko is crashing.
static const char* GetCrashReason() {
  return gMozCrashReason;
}

// Do any special Gecko configuration to get it ready for recording/replaying.
static void ConfigureGecko() {
  // Don't create a stylo thread pool when recording or replaying.
  putenv((char*)"STYLO_THREADS=1");

  // This mutex needs to be initialized on a consistent thread.
  image::RecordReplayInitializeSurfaceCacheMutex();

  // Order statically allocated mutex in intl code.
  RecordReplayOrderDefaultTimeZoneMutex();
}

static const char* GetPlatformKind() {
#if defined(XP_MACOSX)
  return "macOS";
#elif defined(XP_LINUX)
  return "linux";
#else
  return "unknown";
#endif
}

extern char gRecordReplayDriver[];
extern int gRecordReplayDriverSize;

static void* OpenDriverHandle() {
  const char* driver = getenv("RECORD_REPLAY_DRIVER");
  bool temporaryDriver = false;

  if (!driver) {
    const char* tmpdir = getenv("TMPDIR");
    if (!tmpdir) {
      tmpdir = "/tmp";
    }

    char filename[1024];
    snprintf(filename, sizeof(filename), "%s/recordreplay.so-XXXXXX", tmpdir);
    int fd = mkstemp(filename);
    if (fd < 0) {
      fprintf(stderr, "mkstemp failed, can't create driver.\n");
      return nullptr;
    }

    int nbytes = write(fd, gRecordReplayDriver, gRecordReplayDriverSize);
    if (nbytes != gRecordReplayDriverSize) {
      fprintf(stderr, "write to driver temporary file failed, can't create driver.\n");
      return nullptr;
    }

    temporaryDriver = true;
    driver = strdup(filename);
    close(fd);

#ifdef XP_MACOSX
    // Strip any quarantine flag on the written file, if necessary, so that
    // the file can be run or loaded into a process. macOS quarantines any
    // files created by the browser even if they are related to the update
    // process.
    char* args[] = {
      (char*)"/usr/bin/xattr",
      (char*)"-d",
      (char*)"com.apple.quarantine",
      strdup(driver),
    };
    pid_t pid;
    LaunchChildMac(4, args, &pid);
#endif
  }

  void* handle = dlopen(driver, RTLD_LAZY);

  if (temporaryDriver) {
    unlink(driver);
  }

  return handle;
}

bool gRecordAllContent;

extern "C" {

MOZ_EXPORT void RecordReplayInterface_Initialize(int* aArgc, char*** aArgv) {
  // Parse command line options for the process kind and recording file.
  Maybe<const char*> dispatchAddress;
  int argc = *aArgc;
  char** argv = *aArgv;
  for (int i = 0; i < argc; i++) {
    if (!strcmp(argv[i], "-recordReplayDispatch")) {
      MOZ_RELEASE_ASSERT(dispatchAddress.isNothing() && i + 1 < argc);
      const char* arg = argv[i + 1];

      // The special dispatch address "*" is used to indicate that we should
      // save the recording itself to disk.
      dispatchAddress.emplace(strcmp(arg, "*") ? arg : nullptr);
    }
  }
  MOZ_RELEASE_ASSERT(dispatchAddress.isSome());

  Maybe<std::string> apiKey;
  const char* val = getenv("RECORD_REPLAY_API_KEY");
  if (val) {
    apiKey.emplace(val);
    // Unsetting the env var will make the variable unavailable via
    // getenv and such, and also mutates the 'environ' global, so
    // by the time gAttach runs, it will have no idea that this value
    // existed and won't capture it in the recording itself, which
    // is ideal for security.
    MOZ_RELEASE_ASSERT(!unsetenv("RECORD_REPLAY_API_KEY"));
  }

  gDriverHandle = OpenDriverHandle();
  if (!gDriverHandle) {
    fprintf(stderr, "Loading driver failed, crashing.\n");
    MOZ_CRASH("RECORD_REPLAY_DRIVER loading failed");
  }

  LoadSymbol("RecordReplayAttach", gAttach);
  LoadSymbol("RecordReplaySetApiKey", gSetApiKey);
  LoadSymbol("RecordReplayRecordCommandLineArguments",
             gRecordCommandLineArguments);
  LoadSymbol("RecordReplayValue", gRecordReplayValue);
  LoadSymbol("RecordReplayBytes", gRecordReplayBytes);
  LoadSymbol("RecordReplayPrint", gPrintVA);
  LoadSymbol("RecordReplaySaveRecording", gSaveRecording);
  LoadSymbol("RecordReplayFinishRecording", gFinishRecording);
  LoadSymbol("RecordReplayRegisterPointer", gRegisterPointer);
  LoadSymbol("RecordReplayUnregisterPointer", gUnregisterPointer);
  LoadSymbol("RecordReplayPointerId", gPointerId);
  LoadSymbol("RecordReplayAssert", gAssert);
  LoadSymbol("RecordReplayAssertBytes", gAssertBytes);
  LoadSymbol("RecordReplayProgressCounter", gProgressCounter);
  LoadSymbol("RecordReplaySetProgressCallback", gSetProgressCallback, /* aOptional */ true);
  LoadSymbol("RecordReplayProgressReached", gProgressReached, /* aOptional */ true);
  LoadSymbol("RecordReplayBeginPassThroughEvents", gBeginPassThroughEvents);
  LoadSymbol("RecordReplayEndPassThroughEvents", gEndPassThroughEvents);
  LoadSymbol("RecordReplayAreEventsPassedThrough", gAreEventsPassedThrough);
  LoadSymbol("RecordReplayBeginDisallowEvents", gBeginDisallowEvents);
  LoadSymbol("RecordReplayEndDisallowEvents", gEndDisallowEvents);
  LoadSymbol("RecordReplayAreEventsDisallowed", gAreEventsDisallowed);
  LoadSymbol("RecordReplayHasDivergedFromRecording", gHasDivergedFromRecording);
  LoadSymbol("RecordReplayIsUnhandledDivergenceAllowed", gIsUnhandledDivergenceAllowed);
  LoadSymbol("RecordReplayNewCheckpoint", gRecordReplayNewCheckpoint);
  LoadSymbol("RecordReplayIsReplaying", gRecordReplayIsReplaying);
  LoadSymbol("RecordReplayCreateOrderedLock", gCreateOrderedLock);
  LoadSymbol("RecordReplayOrderedLock", gOrderedLock);
  LoadSymbol("RecordReplayOrderedUnlock", gOrderedUnlock);
  LoadSymbol("RecordReplayAddOrderedPthreadMutex", gAddOrderedPthreadMutex);
  LoadSymbol("RecordReplayOnMouseEvent", gOnMouseEvent);
  LoadSymbol("RecordReplayOnKeyEvent", gOnKeyEvent);
  LoadSymbol("RecordReplaySetRecordingIdCallback", gSetRecordingIdCallback);
  LoadSymbol("RecordReplayProcessRecording", gProcessRecording);
  LoadSymbol("RecordReplaySetCrashReasonCallback", gSetCrashReasonCallback);
  LoadSymbol("RecordReplayInvalidateRecording", gInvalidateRecording);
  LoadSymbol("RecordReplaySetCrashNote", gSetCrashNote, /* aOptional */ true);

  if (apiKey) {
    gSetApiKey(apiKey->c_str());
  }

  char buildId[128];
  snprintf(buildId, sizeof(buildId), "%s-gecko-%s", GetPlatformKind(), PlatformBuildID());
  gAttach(*dispatchAddress, buildId);

  if (TestEnv("RECORD_ALL_CONTENT")) {
    gRecordAllContent = true;

    // We only save information about the recording to disk when recording all
    // content. We don't want to save this information when the user explicitly
    // started recording --- they won't use the recording CLI tool
    // (https://github.com/RecordReplay/recordings-cli) afterwards to inspect
    // the recording, and we don't want to leak recording IDs to disk in an
    // unexpected way.
    if (gSaveRecording) {
      gSaveRecording(nullptr);
    }
  }

  js::InitializeJS();
  InitializeGraphics();

  gIsRecordingOrReplaying = true;
  gIsRecording = !gRecordReplayIsReplaying();
  gIsReplaying = gRecordReplayIsReplaying();

  const char* logFile = getenv("RECORD_REPLAY_CRASH_LOG");
  if (logFile) {
    void (*SetCrashLogFile)(const char*);
    LoadSymbol("RecordReplaySetCrashLogFile", SetCrashLogFile);
    SetCrashLogFile(logFile);
  }

  ParseJSFilters("RECORD_REPLAY_RECORD_EXECUTION_ASSERTS", gExecutionAsserts);
  ParseJSFilters("RECORD_REPLAY_RECORD_JS_ASSERTS", gJSAsserts);

  gRecordCommandLineArguments(aArgc, aArgv);
  gSetRecordingIdCallback(RecordingIdCallback);
  gSetCrashReasonCallback(GetCrashReason);

  // Unless disabled via the environment, pre-process all created recordings so
  // that they will load faster after saving the recording.
  if (!TestEnv("RECORD_REPLAY_DONT_PROCESS_RECORDINGS") &&
      !TestEnv("RECORD_ALL_CONTENT")) {
    gProcessRecording();
  }

  ConfigureGecko();
}

MOZ_EXPORT size_t
RecordReplayInterface_InternalRecordReplayValue(const char* aWhy, size_t aValue) {
  return gRecordReplayValue(aWhy, aValue);
}

MOZ_EXPORT void RecordReplayInterface_InternalRecordReplayBytes(const char* aWhy,
                                                                void* aData,
                                                                size_t aSize) {
  gRecordReplayBytes(aWhy, aData, aSize);
}

MOZ_EXPORT void RecordReplayInterface_InternalInvalidateRecording(
    const char* aWhy) {
  gInvalidateRecording("%s", aWhy);
}

MOZ_EXPORT void RecordReplayInterface_InternalRecordReplayAssert(
    const char* aFormat, va_list aArgs) {
  gAssert(aFormat, aArgs);
}

MOZ_EXPORT void RecordReplayInterface_InternalRecordReplayAssertBytes(
    const void* aData, size_t aSize) {
  gAssertBytes("Bytes", aData, aSize);
}

MOZ_EXPORT void RecordReplayAssertFromC(const char* aFormat, ...) {
  if (IsRecordingOrReplaying()) {
    va_list args;
    va_start(args, aFormat);
    gAssert(aFormat, args);
    va_end(args);
  }
}

MOZ_EXPORT void RecordReplayInterface_InternalRegisterThing(void* aThing) {
  gRegisterPointer(aThing);
}

MOZ_EXPORT void RecordReplayInterface_InternalUnregisterThing(void* aThing) {
  gUnregisterPointer(aThing);
}

MOZ_EXPORT size_t RecordReplayInterface_InternalThingIndex(void* aThing) {
  return gPointerId(aThing);
}

MOZ_EXPORT void RecordReplayInterface_InternalHoldJSObject(void* aJSObj) {
  if (aJSObj) {
    JSContext* cx = dom::danger::GetJSContext();
    JS::PersistentRootedObject* root = new JS::PersistentRootedObject(cx);
    *root = static_cast<JSObject*>(aJSObj);
  }
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

MOZ_EXPORT void RecordReplayInterface_ExecutionProgressHook(unsigned aSourceId, const char* aFilename, unsigned aLineno,
                                                            unsigned aColumn) {
  if (FilterMatches(gExecutionAsserts, aFilename, aLineno)) {
    RecordReplayAssert("ExecutionProgress %u:%s:%u:%u", aSourceId, aFilename, aLineno, aColumn);
  }
}

MOZ_EXPORT bool RecordReplayInterface_ShouldEmitRecordReplayAssert(const char* aFilename,
                                                                   unsigned aLineno,
                                                                   unsigned aColumn) {
  return FilterMatches(gJSAsserts, aFilename, aLineno);
}

MOZ_EXPORT void RecordReplayInterface_InternalPrintLog(const char* aFormat,
                                                       va_list aArgs) {
  gPrintVA(aFormat, aArgs);
}

MOZ_EXPORT ProgressCounter* RecordReplayInterface_ExecutionProgressCounter() {
  return gProgressCounter();
}

MOZ_EXPORT void RecordReplayInterface_AdvanceExecutionProgressCounter() {
  ++*gProgressCounter();
}

MOZ_EXPORT void RecordReplayInterface_SetExecutionProgressCallback(void (*aCallback)(uint64_t)) {
  if (gSetProgressCallback) {
    gSetProgressCallback(aCallback);
  }
}

MOZ_EXPORT void RecordReplayInterface_ExecutionProgressReached() {
  gProgressReached();
}

MOZ_EXPORT void RecordReplayInterface_InternalBeginPassThroughThreadEvents() {
  gBeginPassThroughEvents();
}

MOZ_EXPORT void RecordReplayInterface_InternalEndPassThroughThreadEvents() {
  gEndPassThroughEvents();
}

MOZ_EXPORT bool RecordReplayInterface_InternalAreThreadEventsPassedThrough() {
  return gAreEventsPassedThrough();
}

MOZ_EXPORT void RecordReplayInterface_InternalBeginDisallowThreadEvents() {
  gBeginDisallowEvents();
}

MOZ_EXPORT void RecordReplayInterface_InternalEndDisallowThreadEvents() {
  gEndDisallowEvents();
}

MOZ_EXPORT bool RecordReplayInterface_InternalAreThreadEventsDisallowed() {
  return gAreEventsDisallowed();
}

MOZ_EXPORT bool RecordReplayInterface_InternalHasDivergedFromRecording() {
  return gHasDivergedFromRecording();
}

MOZ_EXPORT bool RecordReplayInterface_InternalIsUnhandledDivergenceAllowed() {
  return gIsUnhandledDivergenceAllowed();
}

MOZ_EXPORT int RecordReplayInterface_InternalCreateOrderedLock(const char* aName) {
  return gCreateOrderedLock(aName);
}

int RecordReplayCreateOrderedLock(const char* aName) {
  if (gCreateOrderedLock) {
    return gCreateOrderedLock(aName);
  }
  return 0;
}

MOZ_EXPORT void RecordReplayInterface_InternalOrderedLock(int aLock) {
  gOrderedLock(aLock);
}

void RecordReplayOrderedLock(int aLock) {
  if (gOrderedLock) {
    gOrderedLock(aLock);
  }
}

MOZ_EXPORT void RecordReplayInterface_InternalOrderedUnlock(int aLock) {
  gOrderedUnlock(aLock);
}

void RecordReplayOrderedUnlock(int aLock) {
  if (gOrderedUnlock) {
    gOrderedUnlock(aLock);
  }
}

MOZ_EXPORT void RecordReplayInterface_InternalAddOrderedPthreadMutex(const char* aName,
                                                                     pthread_mutex_t* aMutex) {
  gAddOrderedPthreadMutex(aName, aMutex);
}

static Vector<const char*> gCrashNotes;

MOZ_EXPORT void RecordReplayInterface_InternalPushCrashNote(const char* aNote) {
  if (NS_IsMainThread()) {
    (void) gCrashNotes.append(aNote);
    if (gSetCrashNote) {
      gSetCrashNote(aNote);
    }
  }
}

MOZ_EXPORT void RecordReplayInterface_InternalPopCrashNote() {
  if (NS_IsMainThread()) {
    MOZ_RELEASE_ASSERT(gCrashNotes.length());
    gCrashNotes.popBack();
    if (gSetCrashNote) {
      gSetCrashNote(gCrashNotes.length() ? gCrashNotes.back() : nullptr);
    }
  }
}

}  // extern "C"

static void ParseJSFilters(const char* aEnv, InfallibleVector<JSFilter>& aFilters) {
  const char* value = getenv(aEnv);
  if (!value) {
    return;
  }

  if (!strcmp(value, "*")) {
    JSFilter filter;
    filter.mFilename = value;
    aFilters.append(filter);
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

    PrintLog("ParseJSFilter %s %s %u %u", aEnv,
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
    if (filter.mFilename == "*") {
      return true;
    }
    if (strstr(aFilename, filter.mFilename.c_str()) &&
        aLine >= filter.mStartLine &&
        aLine <= filter.mEndLine) {
      return true;
    }
  }
  return false;
}

const char* CurrentFirefoxVersion() {
  return "74.0a1";
}

static bool gHasCheckpoint = false;

bool HasCheckpoint() {
  return gHasCheckpoint;
}

void CreateCheckpoint() {
  js::EnsureModuleInitialized();
  js::MaybeSendRecordingUnusable();

  gRecordReplayNewCheckpoint();
  gHasCheckpoint = true;
}

void MaybeCreateCheckpoint() {
  // This is called at the top of the event loop, and the process might not be
  // fully initialized. CreateCheckpoint() is only called after the process has
  // been fully initialized, and we don't want any checkpoints before then.
  if (HasCheckpoint()) {
    gRecordReplayNewCheckpoint();
  }
}

static bool gTearingDown;

void FinishRecording() {
  js::SendRecordingFinished();

  gFinishRecording();

  // RecordReplayFinishRecording() does not return until the recording has been
  // fully uploaded. The ContentParent will not kill this process after
  // finishing the recording, so we have to it ourselves.
  PrintLog("Recording finished, exiting.");

  // Use abort to avoid running static initializers.
  gTearingDown = true;
  abort();
}

bool IsTearingDownProcess() {
  return gTearingDown;
}

void OnMouseEvent(dom::BrowserChild* aChild, const WidgetMouseEvent& aEvent) {
  if (!gHasCheckpoint) {
    return;
  }

  const char* kind = nullptr;
  if (aEvent.mMessage == eMouseDown) {
    kind = "mousedown";
  } else if (aEvent.mMessage == eMouseMove) {
    kind = "mousemove";
  }

  if (kind) {
    gOnMouseEvent(kind, aEvent.mRefPoint.x, aEvent.mRefPoint.y);
  }
}

void OnKeyboardEvent(dom::BrowserChild* aChild, const WidgetKeyboardEvent& aEvent) {
  if (!gHasCheckpoint) {
    return;
  }

  const char* kind = nullptr;
  if (aEvent.mMessage == eKeyPress) {
    kind = "keypress";
  } else if (aEvent.mMessage == eKeyDown) {
    kind = "keydown";
  } else if (aEvent.mMessage == eKeyUp) {
    kind = "keyup";
  }

  if (kind) {
    nsAutoString key;
    aEvent.GetDOMKeyName(key);

    gOnKeyEvent(kind, PromiseFlatCString(NS_ConvertUTF16toUTF8(key)).get());
  }
}

static void RecordingIdCallback(const char* aRecordingId) {
  // Print out a string that is recognized by the automated test harness.
  AutoPassThroughThreadEvents pt;
  const char* url = getenv("RECORD_REPLAY_URL");
  fprintf(stderr, "CreateRecording %s %s\n", aRecordingId, url ? url : "");
}

}  // namespace recordreplay
}  // namespace mozilla
