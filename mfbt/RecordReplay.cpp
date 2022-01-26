/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "RecordReplay.h"

#include "js/GCAnnotations.h"
#include "mozilla/Atomics.h"
#include "mozilla/Casting.h"
#include "mozilla/Utf8.h"

#ifndef XP_WIN
#include <dlfcn.h>
#else
#include <windows.h>
#include <libloaderapi.h>
#include <psapi.h>
#endif

#include <stdlib.h>

namespace mozilla {
namespace recordreplay {

// clang-format off
#define FOR_EACH_INTERFACE(Macro)                                              \
  Macro(InternalAreThreadEventsPassedThrough, bool, (), ())                    \
  Macro(InternalAreThreadEventsDisallowed, bool, (), ())                       \
  Macro(InternalRecordReplayValue, size_t, (const char* aWhy, size_t aValue),  \
        (aWhy, aValue))                                                        \
  Macro(InternalHasDivergedFromRecording, bool, (), ())                        \
  Macro(InternalIsUnhandledDivergenceAllowed, bool, (), ())                    \
  Macro(InternalThingIndex, size_t, (void* aThing), (aThing))                  \
  Macro(InternalIndexThing, void*, (size_t aId), (aId))                        \
  Macro(InternalCreateOrderedLock, int, (const char* aName), (aName))          \
  Macro(ExecutionProgressCounter, ProgressCounter*, (), ())                    \
  Macro(NewTimeWarpTarget, ProgressCounter, (), ())                            \
  Macro(ShouldUpdateProgressCounter, bool, (const char* aURL), (aURL))

#ifndef XP_WIN
#define FOR_EACH_PLATFORM_INTERFACE_VOID(Macro)                                \
  Macro(InternalAddOrderedPthreadMutex,                                        \
        (const char* aName, pthread_mutex_t* aMutex), (aName, aMutex))
#else
#define FOR_EACH_PLATFORM_INTERFACE_VOID(Macro)                                \
  Macro(InternalAddOrderedCriticalSection,                                     \
        (const char* aName, void* aCS), (aName, aCS))                          \
  Macro(InternalAddOrderedSRWLock,                                             \
        (const char* aName, void* aLock), (aName, aLock))
#endif

#define FOR_EACH_INTERFACE_VOID(Macro)                                         \
  FOR_EACH_PLATFORM_INTERFACE_VOID(Macro)                                      \
  Macro(InternalBeginPassThroughThreadEvents, (), ())                          \
  Macro(InternalEndPassThroughThreadEvents, (), ())                            \
  Macro(InternalBeginDisallowThreadEvents, (), ())                             \
  Macro(InternalEndDisallowThreadEvents, (), ())                               \
  Macro(InternalPushCrashNote, (const char* aNote), (aNote))                   \
  Macro(InternalPopCrashNote, (), ())                                          \
  Macro(InternalRecordReplayBytes, (const char* aWhy, void* aData, size_t aSize), \
        (aWhy, aData, aSize))                                                  \
  Macro(InternalInvalidateRecording, (const char* aWhy), (aWhy))               \
  Macro(InternalRecordReplayAssert, (const char* aFormat, va_list aArgs),      \
        (aFormat, aArgs))                                                      \
  Macro(InternalRecordReplayAssertBytes, (const void* aData, size_t aSize),    \
        (aData, aSize))                                                        \
  Macro(InternalPrintLog, (const char* aFormat, va_list aArgs),                \
        (aFormat, aArgs))                                                      \
  Macro(InternalDiagnostic, (const char* aFormat, va_list aArgs),              \
        (aFormat, aArgs))                                                      \
  Macro(InternalRegisterThing, (void* aThing), (aThing))                       \
  Macro(InternalUnregisterThing, (void* aThing), (aThing))                     \
  Macro(InternalOrderedLock, (int aLock), (aLock))                             \
  Macro(InternalOrderedUnlock, (int aLock), (aLock))                           \
  Macro(BeginContentParse,                                                     \
        (const void* aToken, const char* aURL, const char* aContentType),      \
        (aToken, aURL, aContentType))                                          \
  Macro(AddContentParseData8,                                                  \
        (const void* aToken, const mozilla::Utf8Unit* aUtf8Buffer,             \
         size_t aLength),                                                      \
        (aToken, aUtf8Buffer, aLength))                                        \
  Macro(AddContentParseData16,                                                 \
        (const void* aToken, const char16_t* aBuffer, size_t aLength),         \
        (aToken, aBuffer, aLength))                                            \
  Macro(EndContentParse, (const void* aToken), (aToken))                       \
  Macro(ReportUnsupportedFeature,                                              \
        (const char* aFeature, int aIssueNumber), (aFeature, aIssueNumber))    \
  Macro(AdvanceExecutionProgressCounter, (), ())                               \
  Macro(SetExecutionProgressCallback, (void (*aCallback)(uint64_t)), (aCallback)) \
  Macro(ExecutionProgressReached, (), ())                                      \
  Macro(InternalAssertScriptedCaller, (const char* aWhy), (aWhy))              \
  Macro(InternalNotifyActivity, (), ())                                        \
  Macro(AddProfilerEvent, (const char* aEvent, const char* aJSON), (aEvent, aJSON))
// clang-format on

#define DECLARE_SYMBOL(aName, aReturnType, aFormals, _) \
  static aReturnType(*gPtr##aName) aFormals;
#define DECLARE_SYMBOL_VOID(aName, aFormals, _) \
  DECLARE_SYMBOL(aName, void, aFormals, _)

FOR_EACH_INTERFACE(DECLARE_SYMBOL)
FOR_EACH_INTERFACE_VOID(DECLARE_SYMBOL_VOID)

#undef DECLARE_SYMBOL
#undef DECLARE_SYMBOL_VOID

static void* LoadSymbol(const char* aName) {
#ifdef XP_WIN
  static HMODULE module;
  if (!module) {
    module = GetModuleHandle("xul.dll");
    if (!module) {
      fprintf(stderr, "Could not find libxul.dll in loaded modules, crashing...\n");
      MOZ_CRASH("Unexpected modules");
    }
  }
  void* rv = BitwiseCast<void*>(GetProcAddress(module, aName));
#else
  void* rv = dlsym(RTLD_DEFAULT, aName);
#endif
  if (!rv) {
    fprintf(stderr, "Record/Replay LoadSymbol failed: %s\n", aName);
    MOZ_CRASH("LoadSymbol");
  }
  return rv;
}

static bool gInitialized;

void Initialize(int* aArgc, char*** aArgv) {
  if (gInitialized) {
    return;
  }
  gInitialized = true;

  void (*initialize)(int*, char***);
  BitwiseCast(LoadSymbol("RecordReplayInterface_Initialize"), &initialize);
  if (!initialize) {
    return;
  }

#define INIT_SYMBOL(aName, _1, _2, _3) \
  BitwiseCast(LoadSymbol("RecordReplayInterface_" #aName), &gPtr##aName);
#define INIT_SYMBOL_VOID(aName, _2, _3) INIT_SYMBOL(aName, void, _2, _3)

  FOR_EACH_INTERFACE(INIT_SYMBOL)
  FOR_EACH_INTERFACE_VOID(INIT_SYMBOL_VOID)

#undef INIT_SYMBOL
#undef INIT_SYMBOL_VOID

  initialize(aArgc, aArgv);
}

// Record/replay API functions can't GC, but we can't use
// JS::AutoSuppressGCAnalysis here due to linking issues.
struct AutoSuppressGCAnalysis {
  AutoSuppressGCAnalysis() {}
  ~AutoSuppressGCAnalysis() {
#ifdef DEBUG
    // Need nontrivial destructor.
    static Atomic<int, SequentiallyConsistent> dummy;
    dummy++;
#endif
  }
} JS_HAZ_GC_SUPPRESSED;

#define DEFINE_WRAPPER(aName, aReturnType, aFormals, aActuals) \
  aReturnType aName aFormals {                                 \
    AutoSuppressGCAnalysis suppress;                           \
    return gPtr##aName aActuals;                               \
  }

#define DEFINE_WRAPPER_VOID(aName, aFormals, aActuals)     \
  void aName aFormals {                                    \
    AutoSuppressGCAnalysis suppress;                       \
    gPtr##aName aActuals;                                  \
  }

FOR_EACH_INTERFACE(DEFINE_WRAPPER)
FOR_EACH_INTERFACE_VOID(DEFINE_WRAPPER_VOID)

#undef DEFINE_WRAPPER
#undef DEFINE_WRAPPER_VOID

bool gIsRecordingOrReplaying;
bool gIsRecording;
bool gIsReplaying;
bool gIsProfiling;

}  // namespace recordreplay
}  // namespace mozilla
