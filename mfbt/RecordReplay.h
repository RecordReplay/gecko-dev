/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Public API for recording/replaying. */

#ifndef mozilla_RecordReplay_h
#define mozilla_RecordReplay_h

#include "mozilla/AlreadyAddRefed.h"
#include "mozilla/Atomics.h"
#include "mozilla/Attributes.h"
#include "mozilla/TemplateLib.h"
#include "mozilla/Types.h"
#include "mozilla/Utf8.h"

#include <functional>
#include <stdarg.h>

class nsIHttpChannel;
class nsIInputStream;
class nsIStreamListener;
class nsIURI;

namespace mozilla {

class WidgetMouseEvent;
class WidgetKeyboardEvent;
namespace dom { class BrowserChild; }

namespace recordreplay {

// Record/Replay Overview.
//
// Firefox content processes can be specified to record or replay their
// behavior. Whether a process is recording or replaying is initialized at the
// start of the main() routine, and is afterward invariant for the process.
//
// Recording and replaying works by controlling non-determinism in the browser:
// non-deterministic behaviors are initially recorded, then later replayed
// exactly to force the browser to behave deterministically. Two types of
// non-deterministic behaviors are captured: intra-thread and inter-thread.
// Intra-thread non-deterministic behaviors are non-deterministic even in the
// absence of actions by other threads, and inter-thread non-deterministic
// behaviors are those affected by interleaving execution with other threads.
//
// Intra-thread non-determinism is recorded and replayed as a stream of events
// for each thread. Most events originate from calls to system library
// functions (for i/o and such); the record/replay system handles these
// internally by redirecting these library functions so that code can be
// injected and the event recorded/replayed. Events can also be manually
// performed using the RecordReplayValue and RecordReplayBytes APIs below.
//
// Inter-thread non-determinism is recorded and replayed by keeping track of
// the order in which threads acquire locks or perform atomic accesses. If the
// program is data race free, then reproducing the order of these operations
// will give an interleaving that is functionally (if not exactly) the same
// as during the recording. As for intra-thread non-determinism, system library
// redirections are used to capture most inter-thread non-determinism, but the
// {Begin,End}OrderedAtomicAccess APIs below can be used to add new ordering
// constraints.
//
// Some behaviors can differ between recording and replay. Mainly, pointer
// values can differ, and JS GCs can occur at different points (a more complete
// list is at the URL below). Some of the APIs below are used to accommodate
// these behaviors and keep the replaying process on track.
//
// This file contains the main public API for places where mozilla code needs
// to interact with the record/replay system.

///////////////////////////////////////////////////////////////////////////////
// Public API
///////////////////////////////////////////////////////////////////////////////

extern MFBT_DATA bool gIsRecordingOrReplaying;
extern MFBT_DATA bool gIsRecording;
extern MFBT_DATA bool gIsReplaying;
extern MFBT_DATA bool gIsProfiling;

// Get the kind of recording/replaying process this is, if any.
static inline bool IsRecordingOrReplaying() { return gIsRecordingOrReplaying; }
static inline bool IsRecording() { return gIsRecording; }
static inline bool IsReplaying() { return gIsReplaying; }

// Return whether execution is being profiled. This does not imply the process
// is recording/replaying.
static inline bool IsProfiling() { return gIsProfiling; }

// Mark a region where thread events are passed through the record/replay
// system. While recording, no information from system calls or other events
// will be recorded for the thread. While replaying, system calls and other
// events are performed normally.
static inline void BeginPassThroughThreadEvents();
static inline void EndPassThroughThreadEvents();

// Whether events in this thread are passed through.
static inline bool AreThreadEventsPassedThrough();

// RAII class for regions where thread events are passed through.
struct MOZ_RAII AutoPassThroughThreadEvents {
  AutoPassThroughThreadEvents() { BeginPassThroughThreadEvents(); }
  ~AutoPassThroughThreadEvents() { EndPassThroughThreadEvents(); }
};

// Mark a region where thread events are not allowed to occur. The process will
// crash immediately if an event does happen.
static inline void BeginDisallowThreadEvents();
static inline void EndDisallowThreadEvents();

// Whether events in this thread are disallowed.
static inline bool AreThreadEventsDisallowed();

// RAII class for a region where thread events are disallowed.
struct MOZ_RAII AutoDisallowThreadEvents {
  AutoDisallowThreadEvents() { BeginDisallowThreadEvents(); }
  ~AutoDisallowThreadEvents() { EndDisallowThreadEvents(); }
};

// Mark a region where a note will be printed when crashing.
static inline void PushCrashNote(const char* aNote);
static inline void PopCrashNote();

// RAII class for a region where a note will be printed when crashing.
struct MOZ_RAII AutoSetCrashNote {
  AutoSetCrashNote(const char* aNote) { PushCrashNote(aNote); }
  ~AutoSetCrashNote() { PopCrashNote(); }
};

// Record or replay a value in the current thread's event stream.
static inline size_t RecordReplayValue(const char* aWhy, size_t aValue);

// Record or replay the contents of a range of memory in the current thread's
// event stream.
static inline void RecordReplayBytes(const char* aWhy, void* aData, size_t aSize);

// During recording or replay, mark the recording as unusable. There are some
// behaviors that can't be reliably recorded or replayed. For more information,
// see 'Unrecordable Executions' in the URL above.
static inline void InvalidateRecording(const char* aWhy);

// Some devtools operations which execute in a replaying process can cause code
// to run which did not run while recording. For example, the JS debugger can
// run arbitrary JS while paused at a breakpoint, by doing an eval(). In such
// cases we say that execution has diverged from the recording, and if recorded
// events are encountered the associated devtools operation fails. This API can
// be used to test for such cases and avoid causing the operation to fail.
static inline bool HasDivergedFromRecording();

// Return whether execution is allowed to interact with the system in a way
// that could trigger an unhandled divergence. This returns true except during
// certain operations while diverged from the recording.
static inline bool IsUnhandledDivergenceAllowed();

// API for debugging inconsistent behavior between recording and replay.
// By calling Assert or AssertBytes a thread event will be inserted and any
// inconsistent execution order of events will be detected (as for normal
// thread events) and reported to the console.
//
// RegisterThing/UnregisterThing associate arbitrary pointers with indexes that
// will be consistent between recording/replaying and can be used in assertion
// strings.
static inline void RecordReplayAssert(const char* aFormat, ...);
static inline void RecordReplayAssertBytes(const void* aData, size_t aSize);
static inline void RegisterThing(void* aThing);
static inline void UnregisterThing(void* aThing);
static inline size_t ThingIndex(void* aThing);
static inline void* IndexThing(size_t aIndex);

// Access a locking resource that will be acquired in the same order when
// replaying as when recording.
static inline int CreateOrderedLock(const char* aName);
static inline void OrderedLock(int aLock);
static inline void OrderedUnlock(int aLock);

// RAII class for using an ordered lock.
struct MOZ_RAII AutoOrderedLock {
  int mLock;

  AutoOrderedLock(int aLock) : mLock(aLock) { OrderedLock(aLock); }
  ~AutoOrderedLock() { OrderedUnlock(mLock); }
};

// Mark an existing mutex so that locking operations on it will occur in the
// same order when replaying as when recording.
#ifndef XP_WIN
static inline void AddOrderedPthreadMutex(const char* aName,
                                          pthread_mutex_t* aMutex);
#else
static inline void AddOrderedCriticalSection(const char* aName,
                                             /*PCRITICAL_SECTION*/ void* aCS);
static inline void AddOrderedSRWLock(const char* aName,
                                     /*PSRWLOCK*/ void* aLock);
#endif

// Atomic wrapper that ensures accesses happen in the same order when
// recording vs. replaying.
template <typename T, MemoryOrdering Order = SequentiallyConsistent>
struct OrderedAtomic {
  Atomic<T, Order> mInner;
  int mOrderedLockId;

  explicit constexpr OrderedAtomic(T aInit)
    : mInner(aInit),
      mOrderedLockId(recordreplay::CreateOrderedLock("OrderedAtomic"))
  {}

  T operator=(T aVal) {
    OrderedLock(mOrderedLockId);
    mInner = aVal;
    OrderedUnlock(mOrderedLockId);
    return aVal;
  }

  operator T() const {
    OrderedLock(mOrderedLockId);
    T rv = mInner;
    OrderedUnlock(mOrderedLockId);
    return rv;
  }

  T exchange(T aVal) {
    OrderedLock(mOrderedLockId);
    T rv = mInner.exchange(aVal);
    OrderedUnlock(mOrderedLockId);
    return rv;
  }
};

// Determine whether this is a recording/replaying process, and
// initialize record/replay state if so.
MFBT_API void Initialize(int* aArgc, char*** aArgv);

// Get the counter used to keep track of how much progress JS execution has
// made while running on the main thread. Progress must advance whenever a JS
// function is entered or loop entry point is reached, so that no script
// location may be hit twice while the progress counter is the same. See
// JSControl.h for more.
typedef uint64_t ProgressCounter;
MFBT_API ProgressCounter* ExecutionProgressCounter();

// Advance the execution progress counter.
MFBT_API void AdvanceExecutionProgressCounter();

// Set a callback the driver can use to set a destination progress value.
MFBT_API void SetExecutionProgressCallback(void (*aCallback)(uint64_t));

// Called when the last destination progress value which was set has been reached.
MFBT_API void ExecutionProgressReached();

// Get an identifier for the current execution point which can be used to warp
// here later.
MFBT_API ProgressCounter NewTimeWarpTarget();

// Return whether a script should update the progress counter when it runs.
MFBT_API bool ShouldUpdateProgressCounter(const char* aURL);

// Define a RecordReplayControl object on the specified global object, with
// methods specialized to the current recording/replaying process
// kind. "aCx" must be a JSContext* pointer, and "aObj" must be a JSObject*
// pointer.
MFBT_API bool DefineRecordReplayControlObject(void* aCx, void* aObj);

// Notify the infrastructure that some URL which contains JavaScript or CSS is
// being parsed. This is used to provide the complete contents of the URL to
// devtools code when it is inspecting the state of this process; that devtools
// code can't simply fetch the URL itself since it may have been changed since
// the recording was made or may no longer exist. The token for a parse may not
// be used in other parses until after EndContentParse() is called.
MFBT_API void BeginContentParse(const void* aToken, const char* aURL,
                                const char* aContentType);

// Add some UTF-8 parse data to an existing content parse.
MFBT_API void AddContentParseData8(const void* aToken,
                                   const Utf8Unit* aUtf8Buffer, size_t aLength);

// Add some UTF-16 parse data to an existing content parse.
MFBT_API void AddContentParseData16(const void* aToken, const char16_t* aBuffer,
                                    size_t aLength);

// Mark a content parse as having completed.
MFBT_API void EndContentParse(const void* aToken);

// Perform an entire content parse of UTF-8 data.
static inline void NoteContentParse(const void* aToken, const char* aURL,
                                    const char* aContentType,
                                    const Utf8Unit* aUtf8Buffer,
                                    size_t aLength) {
  BeginContentParse(aToken, aURL, aContentType);
  AddContentParseData8(aToken, aUtf8Buffer, aLength);
  EndContentParse(aToken);
}

// Perform an entire content parse of UTF-16 data.
static inline void NoteContentParse(const void* aToken, const char* aURL,
                                    const char* aContentType,
                                    const char16_t* aBuffer, size_t aLength) {
  BeginContentParse(aToken, aURL, aContentType);
  AddContentParseData16(aToken, aBuffer, aLength);
  EndContentParse(aToken);
}

// Add a record/replay assertion for the current JS caller.
static inline void AssertScriptedCaller(const char* aWhy);

// Notify the record/replay driver during long running operations off the main thread.
static inline void NotifyActivity();

// Report that the current recording/replaying process is using an unsupported
// browser feature, and message the user to notify them the page might not work right.
// Issue numbers are from https://github.com/RecordReplay/gecko-dev/issues
MFBT_API void ReportUnsupportedFeature(const char* aFeature, int aIssueNumber);

// Report an event that will be added to any profile the record/replay driver
// is generating.
MFBT_API void AddProfilerEvent(const char* aEvent, const char* aJSON);

// Label executable code with a kind. Used while profiling.
MFBT_API void LabelExecutableCode(const void* aCode, size_t aSize, const char* aKind);

///////////////////////////////////////////////////////////////////////////////
// Gecko interface
///////////////////////////////////////////////////////////////////////////////

// These methods are for use by Gecko, and can't be called from SpiderMonkey
// or non-XUL methods without link errors.

void FinishRecording();
void AddRecordingOperation(const char* aKind, const char* aValue);
void CreateCheckpoint();
void MaybeCreateCheckpoint();
void OnMouseEvent(dom::BrowserChild* aChild, const WidgetMouseEvent& aEvent);
void OnKeyboardEvent(dom::BrowserChild* aChild, const WidgetKeyboardEvent& aEvent);
void OnLocationChange(dom::BrowserChild* aChild, nsIURI* aLocation, uint32_t aFlags);
void OnPaint();
const char* CurrentFirefoxVersion();
const char* GetBuildId();
void OnTestCommand(const char* aString);
void OnRepaintNeeded(const char* aWhy);
bool IsTearingDownProcess();

// API for hash table stability.
typedef bool (*KeyEqualsEntryCallback)(const void* aKey, const void* aEntry, void* aPrivate);
void NewStableHashTable(const void* aTable, KeyEqualsEntryCallback aKeyEqualsEntry, void* aPrivate);
void MoveStableHashTable(const void* aTableSrc, const void* aTableDst);
void DeleteStableHashTable(const void* aTable);
uint32_t LookupStableHashCode(const void* aTable, const void* aKey, uint32_t aUnstableHashCode,
                              bool* aFoundMatch);
void StableHashTableAddEntryForLastLookup(const void* aTable, const void* aEntry);
void StableHashTableMoveEntry(const void* aTable, const void* aEntrySrc, const void* aEntryDst);
void StableHashTableDeleteEntry(const void* aTable, const void* aEntry);

// Wrap a given stream listener to emit an observer notification when the stream
// begins and allow observation of a tee stream.
already_AddRefed<nsIStreamListener> WrapNetworkStreamListener(nsIStreamListener* aListener);

// Wrap a given request input stream to emit an observer notification when the stream
// begins and allow observation of a tee stream.
already_AddRefed<nsIInputStream> WrapNetworkRequestBodyStream(nsIHttpChannel* aChannel,
                                                              nsIInputStream* aStream,
                                                              int64_t aLength);

// Helper to build a JSON object with the given properties.
bool BuildJSON(size_t aNumProperties,
               const char** aPropertyNames, const char** aPropertyValues,
               /*nsCString*/void* aResult);

///////////////////////////////////////////////////////////////////////////////
// API inline function implementation
///////////////////////////////////////////////////////////////////////////////

#  define MOZ_MAKE_RECORD_REPLAY_WRAPPER_VOID(aName, aFormals, aActuals) \
    MFBT_API void Internal##aName aFormals;                              \
    static inline void aName aFormals {                                  \
      if (IsRecordingOrReplaying()) {                                    \
        Internal##aName aActuals;                                        \
      }                                                                  \
    }

#  define MOZ_MAKE_RECORD_REPLAY_WRAPPER(aName, aReturnType, aDefaultValue, \
                                         aFormals, aActuals)                \
    MFBT_API aReturnType Internal##aName aFormals;                          \
    static inline aReturnType aName aFormals {                              \
      if (IsRecordingOrReplaying()) {                                       \
        return Internal##aName aActuals;                                    \
      }                                                                     \
      return aDefaultValue;                                                 \
    }

MOZ_MAKE_RECORD_REPLAY_WRAPPER_VOID(BeginPassThroughThreadEvents, (), ())
MOZ_MAKE_RECORD_REPLAY_WRAPPER_VOID(EndPassThroughThreadEvents, (), ())
MOZ_MAKE_RECORD_REPLAY_WRAPPER(AreThreadEventsPassedThrough, bool, false, (),
                               ())
MOZ_MAKE_RECORD_REPLAY_WRAPPER_VOID(BeginDisallowThreadEvents, (), ())
MOZ_MAKE_RECORD_REPLAY_WRAPPER_VOID(EndDisallowThreadEvents, (), ())
MOZ_MAKE_RECORD_REPLAY_WRAPPER(AreThreadEventsDisallowed, bool, false, (), ())
MOZ_MAKE_RECORD_REPLAY_WRAPPER_VOID(PushCrashNote, (const char* aNote), (aNote))
MOZ_MAKE_RECORD_REPLAY_WRAPPER_VOID(PopCrashNote, (), ())
MOZ_MAKE_RECORD_REPLAY_WRAPPER(RecordReplayValue, size_t, aValue,
                               (const char* aWhy, size_t aValue), (aWhy, aValue))
MOZ_MAKE_RECORD_REPLAY_WRAPPER_VOID(RecordReplayBytes,
                                    (const char* aWhy, void* aData, size_t aSize),
                                    (aWhy, aData, aSize))
MOZ_MAKE_RECORD_REPLAY_WRAPPER(HasDivergedFromRecording, bool, false, (), ())
MOZ_MAKE_RECORD_REPLAY_WRAPPER(IsUnhandledDivergenceAllowed, bool, true, (), ())
MOZ_MAKE_RECORD_REPLAY_WRAPPER_VOID(InvalidateRecording, (const char* aWhy),
                                    (aWhy))
MOZ_MAKE_RECORD_REPLAY_WRAPPER_VOID(RecordReplayAssertBytes,
                                    (const void* aData, size_t aSize),
                                    (aData, aSize))
MOZ_MAKE_RECORD_REPLAY_WRAPPER_VOID(RegisterThing, (void* aThing), (aThing))
MOZ_MAKE_RECORD_REPLAY_WRAPPER_VOID(UnregisterThing, (void* aThing), (aThing))
MOZ_MAKE_RECORD_REPLAY_WRAPPER(ThingIndex, size_t, 0, (void* aThing), (aThing))
MOZ_MAKE_RECORD_REPLAY_WRAPPER(IndexThing, void*, nullptr, (size_t aIndex), (aIndex))
MOZ_MAKE_RECORD_REPLAY_WRAPPER(CreateOrderedLock, int, 0,
                               (const char* aName), (aName))
MOZ_MAKE_RECORD_REPLAY_WRAPPER_VOID(OrderedLock, (int aLock), (aLock))
MOZ_MAKE_RECORD_REPLAY_WRAPPER_VOID(OrderedUnlock, (int aLock), (aLock))
MOZ_MAKE_RECORD_REPLAY_WRAPPER_VOID(AssertScriptedCaller, (const char* aWhy), (aWhy))
MOZ_MAKE_RECORD_REPLAY_WRAPPER_VOID(NotifyActivity, (), ())

#ifndef XP_WIN
MOZ_MAKE_RECORD_REPLAY_WRAPPER_VOID(AddOrderedPthreadMutex,
                                    (const char* aName, pthread_mutex_t* aMutex),
                                    (aName, aMutex));
#else
MOZ_MAKE_RECORD_REPLAY_WRAPPER_VOID(AddOrderedCriticalSection,
                                    (const char* aName, void* aCS),
                                    (aName, aCS));
MOZ_MAKE_RECORD_REPLAY_WRAPPER_VOID(AddOrderedSRWLock,
                                    (const char* aName, void* aLock),
                                    (aName, aLock));
#endif

#undef MOZ_MAKE_RECORD_REPLAY_WRAPPER_VOID
#undef MOZ_MAKERECORDREPLAYWRAPPER

MFBT_API void InternalRecordReplayAssert(const char* aFormat, va_list aArgs);

static inline void RecordReplayAssert(const char* aFormat, ...) {
  if (IsRecordingOrReplaying()) {
    va_list ap;
    va_start(ap, aFormat);
    InternalRecordReplayAssert(aFormat, ap);
    va_end(ap);
  }
}

MFBT_API void InternalPrintLog(const char* aFormat, va_list aArgs);

static inline void PrintLog(const char* aFormat, ...) {
  if (IsRecordingOrReplaying()) {
    va_list ap;
    va_start(ap, aFormat);
    InternalPrintLog(aFormat, ap);
    va_end(ap);
  }
}

MFBT_API void InternalDiagnostic(const char* aFormat, va_list aArgs);

static inline void Diagnostic(const char* aFormat, ...) {
  if (IsRecordingOrReplaying()) {
    va_list ap;
    va_start(ap, aFormat);
    InternalDiagnostic(aFormat, ap);
    va_end(ap);
  }
}

}  // namespace recordreplay
}  // namespace mozilla

#endif /* mozilla_RecordReplay_h */
