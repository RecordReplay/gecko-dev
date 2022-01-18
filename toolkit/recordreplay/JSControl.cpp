/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "JSControl.h"
#include "ProcessRecordReplay.h"

#include "mozilla/Base64.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/StaticPtr.h"
#include "js/CharacterEncoding.h"
#include "js/Conversions.h"
#include "js/JSON.h"
#include "js/PropertySpec.h"
#include "nsImportModule.h"
#include "rrIConnection.h"
#include "rrIModule.h"
#include "xpcprivate.h"
#include "nsMediaFeatures.h"

#include <fcntl.h>
#include <sys/stat.h>

#ifndef XP_WIN
#include <unistd.h>
#endif

using namespace JS;

namespace mozilla {
namespace recordreplay {

// We remember various operations performed by the recording process and add
// them to the vector below, so that JS can get the complete set and send it
// up to the UI process for including in metadata about the recording.
//
// This is used for operations that could be considered security sensitive,
// and is currently targeted at times when the recording accesses existing
// information from the user's profile like cookies and local storage.
struct RecordingOperation {
  nsCString mKind;
  nsCString mValue;

  RecordingOperation(const char* aKind, const char* aValue)
    : mKind(aKind), mValue(aValue) {}
};
static StaticInfallibleVector<RecordingOperation> gRecordingOperations;
static StaticMutex gRecordingOperationsMutex;

void AddRecordingOperation(const char* aKind, const char* aValue) {
  if (!recordreplay::IsRecordingOrReplaying()) {
    return;
  }

  RecordReplayAssert("AddRecordingOperation %s %s", aKind, aValue);

  StaticMutexAutoLock lock(gRecordingOperationsMutex);
  gRecordingOperations.append(RecordingOperation(aKind, aValue));
}

namespace js {

static void (*gOnNewSource)(const char* aId, const char* aKind, const char* aUrl);
static char* (*gGetRecordingId)();
static void (*gSetDefaultCommandCallback)(char* (*aCallback)(const char*, const char*));
static void (*gSetClearPauseDataCallback)(void (*aCallback)());
static void (*gSetChangeInstrumentCallback)(void (*aCallback)(bool));
static void (*gInstrument)(const char* aKind, const char* aFunctionId, int aOffset);
static void (*gOnExceptionUnwind)();
static void (*gOnDebuggerStatement)();
static void (*gOnEvent)(const char* aEvent, bool aBefore);
static void (*gOnNetworkRequest)(const char* aId, const char* aKind, size_t aBookmark);
static void (*gOnNetworkRequestEvent)(const char* aId);
static void (*gOnNetworkStreamStart)(const char* aId, const char* aKind, const char* aParentId);
static void (*gOnNetworkStreamData)(const char* aId, size_t aOffset, size_t aLength, size_t aBookmark);
static void (*gOnNetworkStreamEnd)(const char* aId, size_t aLength);
static void (*gOnConsoleMessage)(int aTimeWarpTarget);
static void (*gOnAnnotation)(const char* aKind, const char* aContents);
static size_t (*gNewTimeWarpTarget)();
static size_t (*gElapsedTimeMs)();
static char* (*gGetUnusableRecordingReason)();
static void (*gAddMetadata)(const char* metadata);

// Callback used when the recording driver is sending us a command to look up
// some state.
static char* CommandCallback(const char* aMethod, const char* aParams);

// Callback used to clear ObjectId associations.
static void ClearPauseDataCallback();

// Callback used to change whether execution is being scanned and we should
// call OnInstrument.
static void ChangeInstrumentCallback(bool aValue);

// Handle initialization at process startup.
void InitializeJS() {
  LoadSymbol("RecordReplayOnNewSource", gOnNewSource);
  LoadSymbol("RecordReplayGetRecordingId", gGetRecordingId);
  LoadSymbol("RecordReplaySetDefaultCommandCallback", gSetDefaultCommandCallback);
  LoadSymbol("RecordReplaySetClearPauseDataCallback", gSetClearPauseDataCallback);
  LoadSymbol("RecordReplaySetChangeInstrumentCallback", gSetChangeInstrumentCallback);
  LoadSymbol("RecordReplayOnInstrument", gInstrument);
  LoadSymbol("RecordReplayOnExceptionUnwind", gOnExceptionUnwind);
  LoadSymbol("RecordReplayOnDebuggerStatement", gOnDebuggerStatement);
  LoadSymbol("RecordReplayOnEvent", gOnEvent);
  LoadSymbol("RecordReplayOnNetworkRequest", gOnNetworkRequest);
  LoadSymbol("RecordReplayOnNetworkRequestEvent", gOnNetworkRequestEvent);
  LoadSymbol("RecordReplayOnNetworkStreamStart", gOnNetworkStreamStart);
  LoadSymbol("RecordReplayOnNetworkStreamData", gOnNetworkStreamData);
  LoadSymbol("RecordReplayOnNetworkStreamEnd", gOnNetworkStreamEnd);
  LoadSymbol("RecordReplayOnConsoleMessage", gOnConsoleMessage);
  LoadSymbol("RecordReplayOnAnnotation", gOnAnnotation);
  LoadSymbol("RecordReplayNewBookmark", gNewTimeWarpTarget);
  LoadSymbol("RecordReplayElapsedTimeMs", gElapsedTimeMs);
  LoadSymbol("RecordReplayGetUnusableRecordingReason", gGetUnusableRecordingReason);
  LoadSymbol("RecordReplayAddMetadata", gAddMetadata);

  gSetDefaultCommandCallback(CommandCallback);
  gSetClearPauseDataCallback(ClearPauseDataCallback);
  gSetChangeInstrumentCallback(ChangeInstrumentCallback);
}

// URL of the root module script.
#define ModuleURL "resource://devtools/server/actors/replay/module.js"

static StaticRefPtr<rrIModule> gModule;
static PersistentRootedObject* gModuleObject;

static bool IsModuleInitialized() {
  return !!gModule;
}

// Interned atoms for the various instrumented operations.
static JSString* gMainAtom;
static JSString* gEntryAtom;
static JSString* gBreakpointAtom;
static JSString* gExitAtom;
static JSString* gGeneratorAtom;

// Handle initialization at the first checkpoint, when we can create JS modules.
void EnsureModuleInitialized() {
  if (IsModuleInitialized()) {
    return;
  }

  // Initialization so we can repaint at the first checkpoint without having
  // an unhandled recording divergence.
  nsMediaFeatures::InitSystemMetrics();

  AutoSafeJSContext cx;
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());

  nsCOMPtr<rrIModule> module = do_ImportModule(ModuleURL);
  gModule = module.forget();
  ClearOnShutdown(&gModule);

  RootedValue value(cx);
  if (NS_FAILED(gModule->Initialize(&value))) {
    MOZ_CRASH("EnsureModuleInitialized: Initialize failed");
  }
  MOZ_RELEASE_ASSERT(value.isObject());

  gModuleObject = new PersistentRootedObject(cx);
  *gModuleObject = &value.toObject();

  gMainAtom = JS_AtomizeAndPinString(cx, "main");
  gEntryAtom = JS_AtomizeAndPinString(cx, "entry");
  gBreakpointAtom = JS_AtomizeAndPinString(cx, "breakpoint");
  gExitAtom = JS_AtomizeAndPinString(cx, "exit");
  gGeneratorAtom = JS_AtomizeAndPinString(cx, "generator");

  MOZ_RELEASE_ASSERT(gMainAtom && gEntryAtom && gBreakpointAtom && gExitAtom && gGeneratorAtom);
}

void ConvertJSStringToCString(JSContext* aCx, JSString* aString,
                              nsAutoCString& aResult) {
  size_t len = JS_GetStringLength(aString);

  nsAutoString chars;
  chars.SetLength(len);
  if (!JS_CopyStringChars(aCx, Range<char16_t>(chars.BeginWriting(), len),
                          aString)) {
    MOZ_CRASH("ConvertJSStringToCString");
  }

  NS_ConvertUTF16toUTF8 utf8(chars);
  aResult = utf8;
}

extern "C" {

MOZ_EXPORT bool RecordReplayInterface_ShouldUpdateProgressCounter(
    const char* aURL) {
  // Progress counters are only updated for scripts which are exposed to the
  // debugger.
  return aURL
      && strncmp(aURL, "resource:", 9)
      && strncmp(aURL, "chrome:", 7)
      && strcmp(aURL, "debugger eval code");
}

}  // extern "C"

extern "C" {

MOZ_EXPORT ProgressCounter RecordReplayInterface_NewTimeWarpTarget() {
  if (AreThreadEventsDisallowed() || !IsModuleInitialized()) {
    return 0;
  }

  return gNewTimeWarpTarget();
}

}  // extern "C"

static void SendUnsupportedFeature(const char* aFeature, int aIssueNumber);

extern "C" {

MOZ_EXPORT void RecordReplayInterface_BeginContentParse(
    const void* aToken, const char* aURL, const char* aContentType) {
  MOZ_RELEASE_ASSERT(IsRecordingOrReplaying());
  MOZ_RELEASE_ASSERT(aToken);
}

MOZ_EXPORT void RecordReplayInterface_AddContentParseData8(
    const void* aToken, const Utf8Unit* aUtf8Buffer, size_t aLength) {
  MOZ_RELEASE_ASSERT(IsRecordingOrReplaying());
  MOZ_RELEASE_ASSERT(aToken);
}

MOZ_EXPORT void RecordReplayInterface_AddContentParseData16(
    const void* aToken, const char16_t* aBuffer, size_t aLength) {
  MOZ_RELEASE_ASSERT(IsRecordingOrReplaying());
  MOZ_RELEASE_ASSERT(aToken);
}

MOZ_EXPORT void RecordReplayInterface_EndContentParse(const void* aToken) {
  MOZ_RELEASE_ASSERT(IsRecordingOrReplaying());
  MOZ_RELEASE_ASSERT(aToken);
}

MOZ_EXPORT void RecordReplayInterface_ReportUnsupportedFeature(const char* aFeature, int aIssueNumber) {
  if (NS_IsMainThread()) {
    SendUnsupportedFeature(aFeature, aIssueNumber);
  } else {
    NS_DispatchToMainThread(NS_NewRunnableFunction("ReportUnsupportedFeature",
                            [=]() { SendUnsupportedFeature(aFeature, aIssueNumber); }));
  }
}

}  // extern "C"

static bool IsRecordingUnusable() {
  if (IsRecording()) {
    return gGetUnusableRecordingReason() != nullptr;
  }
  return false;
}

// Recording IDs are UUIDs, and have a fixed length.
static char gRecordingId[40];
static bool gHasRecordingId;

static const char* GetRecordingId() {
  if (IsRecordingUnusable()) {
    return nullptr;
  }
  if (!gHasRecordingId) {
    // RecordReplayGetRecordingId() is not currently supported while replaying,
    // so we embed the recording ID in the recording itself.
    gHasRecordingId = true;
    if (IsRecording()) {
      char* recordingId = gGetRecordingId();
      if (recordingId) {
        MOZ_RELEASE_ASSERT(*recordingId != 0);
        MOZ_RELEASE_ASSERT(strlen(recordingId) + 1 <= sizeof(gRecordingId));
        strcpy(gRecordingId, recordingId);
      } else {
        memset(gRecordingId, 0, sizeof(gRecordingId));
      }
    }
    RecordReplayBytes("RecordingId", gRecordingId, sizeof(gRecordingId));
  }
  return gRecordingId[0] ? gRecordingId : nullptr;
}

// If we are recording all content processes, whether any interesting content was found.
static bool gHasInterestingContent;

// Call a method exported by the JS module with the given argument.
static void CallModuleMethod(JSContext* cx, const char* aMethod, const char* aArgument, int aArgument2 = 0) {
  JSString* str = JS_NewStringCopyZ(cx, aArgument);
  MOZ_RELEASE_ASSERT(str);

  JS::RootedValueArray<2> args(cx);
  args[0].setString(str);
  args[1].setInt32(aArgument2);

  RootedValue rv(cx);
  if (!JS_CallFunctionName(cx, *gModuleObject, aMethod, args, &rv)) {
    MOZ_CRASH("CallModuleMethod");
  }
}

void SendRecordingUnsupported(const char* aReason) {
  AutoSafeJSContext cx;
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());
  CallModuleMethod(cx, "SendRecordingUnsupported", aReason);
}

static void SendUnsupportedFeature(const char* aFeature, int aIssueNumber) {
  if (!IsModuleInitialized()) {
    return;
  }

  AutoSafeJSContext cx;
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());
  CallModuleMethod(cx, "SendUnsupportedFeature", aFeature, aIssueNumber);
}

// Report the recording as either finished or unusable.
void SendRecordingFinished() {
  // When recording all content, we don't notify the UI process about the
  // new recording. The driver will save information about the recording to disk.
  if (gRecordAllContent) {
    // If we aren't interested in the recording, mark it as unusable
    // so the driver doesn't bother with uploading it.
    if (!gHasInterestingContent) {
      InvalidateRecording("No interesting content");
    }
    return;
  }

  if (!IsModuleInitialized()) {
    return;
  }

  AutoSafeJSContext cx;
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());

  const char* recordingId = GetRecordingId();
  if (!recordingId) {
    char* reason = gGetUnusableRecordingReason();
    MOZ_RELEASE_ASSERT(reason);
    CallModuleMethod(cx, "SendRecordingUnusable", reason);
    return;
  }

  CallModuleMethod(cx, "SendRecordingFinished", recordingId);
}

void MaybeSendRecordingUnusable() {
  MOZ_RELEASE_ASSERT(IsModuleInitialized());

  if (IsRecordingUnusable()) {
    // Finishing the recording after it is unusable will notify the UI process
    // appropriately, and will trigger shutdown of this process appropriately.
    FinishRecording();
  }
}

///////////////////////////////////////////////////////////////////////////////
// Module Interface
///////////////////////////////////////////////////////////////////////////////

// Define the methods which the module uses to interact with the recording driver.

static bool Method_Log(JSContext* aCx, unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  RootedString str(aCx, ToString(aCx, args.get(0)));
  if (!str) {
    return false;
  }

  JS::UniqueChars cstr = JS_EncodeStringToLatin1(aCx, str);
  if (!cstr) {
    return false;
  }

  PrintLog("%s", cstr.get());

  args.rval().setUndefined();
  return true;
}

static bool Method_RecordReplayAssert(JSContext* aCx, unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  RootedString str(aCx, ToString(aCx, args.get(0)));
  if (!str) {
    return false;
  }

  JS::UniqueChars cstr = JS_EncodeStringToLatin1(aCx, str);
  if (!cstr) {
    return false;
  }

  RecordReplayAssert("%s", cstr.get());

  args.rval().setUndefined();
  return true;
}

// Return whether aURL is an interesting source and the recording should be
// remembered if all content processes are being recorded.
static bool IsInterestingSource(const char* aURL) {
  if (!aURL) {
    return false;
  }

  // Prefixes for URLs which are part of the browser and not web content.
  static const char* uninterestingPrefixes[] = {
    "moz-extension://",
    "resource://",
    "chrome://",
  };

  for (const char* prefix : uninterestingPrefixes) {
    if (!strncmp(aURL, prefix, strlen(prefix))) {
      return false;
    }
  }

  return true;
}

static bool Method_OnNewSource(JSContext* aCx, unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  if (!args.get(0).isString() ||
      !args.get(1).isString() ||
      !(args.get(2).isString() || args.get(2).isNull())) {
    JS_ReportErrorASCII(aCx, "Bad arguments");
    return false;
  }

  nsAutoCString id, kind;
  ConvertJSStringToCString(aCx, args.get(0).toString(), id);
  ConvertJSStringToCString(aCx, args.get(1).toString(), kind);

  Maybe<nsAutoCString> url;
  if (args.get(2).isString()) {
    url.emplace();
    ConvertJSStringToCString(aCx, args.get(2).toString(), *url);
  }

  const char* urlRaw = url ? url->get() : nullptr;
  gOnNewSource(id.get(), kind.get(), urlRaw);

  // Check to see if the source matches any URL filter we have.
  if (gRecordAllContent && !gHasInterestingContent && IsInterestingSource(urlRaw)) {
    gHasInterestingContent = true;

    // We found some interesting content, add the recording to the file at this env var.
    char* env = getenv("RECORD_REPLAY_RECORDING_ID_FILE");
    if (env) {
      FILE* file = fopen(env, "a");
      if (file) {
        const char* recordingId = GetRecordingId();
        fprintf(file, "%s\n", recordingId);
        fclose(file);
        fprintf(stderr, "Found content %s, saving recording ID %s\n", urlRaw, recordingId);
      } else {
        fprintf(stderr, "Error: Could not open %s for adding recording ID", env);
      }
    }
  }

  args.rval().setUndefined();
  return true;
}

static bool Method_AreThreadEventsDisallowed(JSContext* aCx,
                                             unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);
  args.rval().setBoolean(AreThreadEventsDisallowed());
  return true;
}

static bool Method_ShouldUpdateProgressCounter(JSContext* aCx,
                                               unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  if (args.get(0).isNull()) {
    args.rval().setBoolean(ShouldUpdateProgressCounter(nullptr));
  } else {
    if (!args.get(0).isString()) {
      JS_ReportErrorASCII(aCx, "Expected string or null as first argument");
      return false;
    }

    nsAutoCString str;
    ConvertJSStringToCString(aCx, args.get(0).toString(), str);
    args.rval().setBoolean(ShouldUpdateProgressCounter(str.get()));
  }

  return true;
}

static bool gScanningScripts;

// This is called by the recording driver to notify us when to start/stop scanning.
static void ChangeInstrumentCallback(bool aValue) {
  MOZ_RELEASE_ASSERT(IsModuleInitialized());

  if (gScanningScripts == aValue) {
    return;
  }
  gScanningScripts = aValue;

  AutoSafeJSContext cx;
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());

  JS::RootedValueArray<1> args(cx);
  args[0].setBoolean(aValue);

  RootedValue rv(cx);
  if (!JS_CallFunctionName(cx, *js::gModuleObject, "SetScanningScripts", args, &rv)) {
    MOZ_CRASH("SetScanningScripts");
  }
}

static bool Method_InstrumentationCallback(JSContext* aCx, unsigned aArgc,
                                           Value* aVp) {
  MOZ_RELEASE_ASSERT(gScanningScripts);
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  if (!args.get(0).isString() || !args.get(1).isNumber() || !args.get(2).isNumber()) {
    JS_ReportErrorASCII(aCx, "Bad parameters");
    return false;
  }

  // The kind string should be an atom which we have captured already.
  JSString* kindStr = args.get(0).toString();

  const char* kind;
  if (kindStr == gBreakpointAtom) {
    kind = "breakpoint";
  } else if (kindStr == gMainAtom) {
    kind = "main";
  } else if (kindStr == gGeneratorAtom) {
    kind = "generator";
  } else if (kindStr == gEntryAtom) {
    kind = "entry";
  } else if (kindStr == gExitAtom) {
    kind = "exit";
  }

  uint32_t script = args.get(1).toNumber();
  uint32_t offset = args.get(2).toNumber();

  if (script) {
    char functionId[32];
    snprintf(functionId, sizeof(functionId), "%u", script);
    gInstrument(kind, functionId, offset);
  }

  args.rval().setUndefined();
  return true;
}

static bool Method_IsScanningScripts(JSContext* aCx, unsigned aArgc,
                                     Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  args.rval().setBoolean(gScanningScripts);
  return true;
}

static bool Method_OnExceptionUnwind(JSContext* aCx, unsigned aArgc,
                                     Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  gOnExceptionUnwind();

  args.rval().setUndefined();
  return true;
}

static bool Method_OnDebuggerStatement(JSContext* aCx, unsigned aArgc,
                                       Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  gOnDebuggerStatement();

  args.rval().setUndefined();
  return true;
}

static bool Method_OnEvent(JSContext* aCx, unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  if (!args.get(0).isString() || !args.get(1).isBoolean()) {
    JS_ReportErrorASCII(aCx, "Bad parameters");
    return false;
  }

  nsAutoCString event;
  ConvertJSStringToCString(aCx, args.get(0).toString(), event);
  bool before = args.get(1).toBoolean();

  gOnEvent(event.get(), before);

  args.rval().setUndefined();
  return true;
}

static bool Method_OnHttpRequest(JSContext* aCx, unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  if (!args.get(0).isString() || !args.get(1).isNumber()) {
    JS_ReportErrorASCII(aCx, "Bad parameters");
    return false;
  }

  nsAutoCString requestId;
  ConvertJSStringToCString(aCx, args.get(0).toString(), requestId);

  double bookmark = args.get(1).toNumber();
  MOZ_RELEASE_ASSERT((uint64_t)bookmark == (uint32_t)bookmark, "bad request bookmark");

  gOnNetworkRequest(requestId.get(), "http", (size_t)bookmark);
  return true;
}

static bool Method_OnHttpRequestEvent(JSContext* aCx, unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  if (!args.get(0).isString()) {
    JS_ReportErrorASCII(aCx, "Bad parameters");
    return false;
  }

  nsAutoCString requestId;
  ConvertJSStringToCString(aCx, args.get(0).toString(), requestId);

  gOnNetworkRequestEvent(requestId.get());
  return true;
}

static bool Method_OnNetworkStreamStart(JSContext* aCx, unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  if (!args.get(0).isString() || !args.get(1).isString() || !args.get(2).isString()) {
    JS_ReportErrorASCII(aCx, "Bad parameters");
    return false;
  }

  nsAutoCString streamId;
  ConvertJSStringToCString(aCx, args.get(0).toString(), streamId);

  nsAutoCString streamKind;
  ConvertJSStringToCString(aCx, args.get(1).toString(), streamKind);

  nsAutoCString streamParentId;
  ConvertJSStringToCString(aCx, args.get(2).toString(), streamParentId);

  gOnNetworkStreamStart(streamId.get(), streamKind.get(), streamParentId.get());
  return true;
}

static bool Method_OnNetworkStreamData(JSContext* aCx, unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  if (!args.get(0).isString() || !args.get(1).isNumber() || !args.get(2).isNumber()) {
    JS_ReportErrorASCII(aCx, "Bad parameters");
    return false;
  }

  nsAutoCString streamId;
  ConvertJSStringToCString(aCx, args.get(0).toString(), streamId);

  double offset = args.get(1).toNumber();
  MOZ_RELEASE_ASSERT((uint64_t)offset == (size_t)offset, "bad offset");

  double length = args.get(2).toNumber();
  MOZ_RELEASE_ASSERT((uint64_t)length == (size_t)length, "bad length");

  gOnNetworkStreamData(streamId.get(), (size_t)offset, (size_t)length, 0);
  return true;
}

static bool Method_OnNetworkStreamEnd(JSContext* aCx, unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  if (!args.get(0).isString() || !args.get(1).isNumber()) {
    JS_ReportErrorASCII(aCx, "Bad parameters");
    return false;
  }

  nsAutoCString streamId;
  ConvertJSStringToCString(aCx, args.get(0).toString(), streamId);

  double length = args.get(1).toNumber();
  MOZ_RELEASE_ASSERT((uint64_t)length == (size_t)length, "bad length");

  gOnNetworkStreamEnd(streamId.get(), (size_t)length);
  return true;
}

static bool Method_MakeBookmark(JSContext* aCx, unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  uint64_t bookmark = gNewTimeWarpTarget();
  // Make sure it won't overflow.
  MOZ_RELEASE_ASSERT(bookmark == (uint32_t)bookmark, "bad make bookmark");

  args.rval().setDouble(bookmark);
  return true;
}

static bool Method_RecordingId(JSContext* aCx, unsigned aArgc,
                                     Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  const char* recordingId = GetRecordingId();
  if (recordingId) {
    JSString* str = JS_NewStringCopyZ(aCx, recordingId);
    if (!str) {
      return false;
    }

    args.rval().setString(str);
  } else {
    args.rval().setNull();
  }
  return true;
}

static bool Method_OnConsoleMessage(JSContext* aCx, unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  if (!args.get(0).isNumber()) {
    JS_ReportErrorASCII(aCx, "Bad parameters");
    return false;
  }

  int target = args.get(0).toNumber();
  gOnConsoleMessage(target);

  args.rval().setUndefined();
  return true;
}

static bool Method_OnAnnotation(JSContext* aCx, unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  if (!args.get(0).isString() || !args.get(1).isString()) {
    JS_ReportErrorASCII(aCx, "Bad parameters");
    return false;
  }

  nsAutoCString kind, contents;
  ConvertJSStringToCString(aCx, args.get(0).toString(), kind);
  ConvertJSStringToCString(aCx, args.get(1).toString(), contents);

  gOnAnnotation(kind.get(), contents.get());

  args.rval().setUndefined();
  return true;
}

static bool FillStringCallback(const char16_t* buf, uint32_t len, void* data) {
  nsCString* str = (nsCString*)data;
  MOZ_RELEASE_ASSERT(str->Length() == 0);
  *str = NS_ConvertUTF16toUTF8(buf, len);
  return true;
}

static bool Method_AddMetadata(JSContext* aCx, unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  if (!args.get(0).isObject()) {
    JS_ReportErrorASCII(aCx, "Bad parameters");
    return false;
  }

  RootedObject obj(aCx, &args.get(0).toObject());

  nsCString str;
  if (!JS::ToJSONMaybeSafely(aCx, obj, FillStringCallback, &str)) {
    return false;
  }

  if (gAddMetadata) {
    gAddMetadata(str.get());
  }

  args.rval().setUndefined();
  return true;
}

static bool Method_RecordingOperations(JSContext* aCx, unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  StaticMutexAutoLock lock(gRecordingOperationsMutex);

  RootedObject rv(aCx, NewArrayObject(aCx, gRecordingOperations.length()));
  if (!rv) {
    return false;
  }

  for (size_t i = 0; i < gRecordingOperations.length(); i++) {
    RootedString kind(aCx, JS_NewStringCopyZ(aCx, gRecordingOperations[i].mKind.get()));
    if (!kind) {
      return false;
    }

    RootedString value(aCx, JS_NewStringCopyZ(aCx, gRecordingOperations[i].mValue.get()));
    if (!value) {
      return false;
    }

    RootedObject elem(aCx, JS_NewObject(aCx, nullptr));
    if (!elem) {
      return false;
    }

    RootedValue kindVal(aCx, StringValue(kind));
    RootedValue valueVal(aCx, StringValue(value));

    if (!JS_SetProperty(aCx, elem, "kind", kindVal) ||
        !JS_SetProperty(aCx, elem, "value", valueVal) ||
        !JS_SetElement(aCx, rv, i, elem)) {
      return false;
    }
  }

  args.rval().setObject(*rv);
  return true;
}

static const JSFunctionSpec gRecordReplayMethods[] = {
  JS_FN("log", Method_Log, 1, 0),
  JS_FN("recordReplayAssert", Method_RecordReplayAssert, 1, 0),
  JS_FN("onNewSource", Method_OnNewSource, 3, 0),
  JS_FN("areThreadEventsDisallowed", Method_AreThreadEventsDisallowed, 0, 0),
  JS_FN("shouldUpdateProgressCounter", Method_ShouldUpdateProgressCounter, 1, 0),
  JS_FN("instrumentationCallback", Method_InstrumentationCallback, 3, 0),
  JS_FN("isScanningScripts", Method_IsScanningScripts, 0, 0),
  JS_FN("onExceptionUnwind", Method_OnExceptionUnwind, 0, 0),
  JS_FN("onDebuggerStatement", Method_OnDebuggerStatement, 0, 0),
  JS_FN("onEvent", Method_OnEvent, 2, 0),
  JS_FN("onHttpRequest", Method_OnHttpRequest, 2, 0),
  JS_FN("onHttpRequestEvent", Method_OnHttpRequestEvent, 1, 0),
  JS_FN("onNetworkStreamStart", Method_OnNetworkStreamStart, 3, 0),
  JS_FN("onNetworkStreamData", Method_OnNetworkStreamData, 3, 0),
  JS_FN("onNetworkStreamEnd", Method_OnNetworkStreamEnd, 2, 0),
  JS_FN("onConsoleMessage", Method_OnConsoleMessage, 1, 0),
  JS_FN("onAnnotation", Method_OnAnnotation, 2, 0),
  JS_FN("recordingId", Method_RecordingId, 0, 0),
  JS_FN("addMetadata", Method_AddMetadata, 1, 0),
  JS_FN("recordingOperations", Method_RecordingOperations, 0, 0),
  JS_FN("makeBookmark", Method_MakeBookmark, 0, 0),
  JS_FS_END
};

static char* CommandCallback(const char* aMethod, const char* aParams) {
  MOZ_RELEASE_ASSERT(js::IsModuleInitialized());

  AutoSafeJSContext cx;
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());

  RootedString method(cx, JS_NewStringCopyZ(cx, aMethod));
  RootedString paramsStr(cx, JS_NewStringCopyZ(cx, aParams));
  MOZ_RELEASE_ASSERT(method && paramsStr);

  RootedValue params(cx);
  if (!JS_ParseJSON(cx, paramsStr, &params)) {
    PrintLog("Error: CommandCallback ParseJSON failed %s %s", aMethod, aParams);
    MOZ_CRASH("CommandCallback");
  }

  JS::RootedValueArray<2> args(cx);
  args[0].setString(method);
  args[1].set(params);

  RootedValue rv(cx);
  if (!JS_CallFunctionName(cx, *js::gModuleObject, "OnProtocolCommand", args, &rv)) {
    PrintLog("Error: CommandCallback failed %s", aMethod);
    MOZ_CRASH("CommandCallback");
  }

  if (!rv.isObject()) {
    return nullptr;
  }

  RootedObject obj(cx, &rv.toObject());

  nsCString str;
  if (!JS::ToJSONMaybeSafely(cx, obj, FillStringCallback, &str)) {
    PrintLog("Error: CommandCallback ToJSON failed");
    MOZ_CRASH("CommandCallback");
  }

  return strdup(str.get());
}

static void ClearPauseDataCallback() {
  MOZ_RELEASE_ASSERT(js::IsModuleInitialized());

  AutoSafeJSContext cx;
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());

  recordreplay::Diagnostic("ClearPauseData %d", JS_IsExceptionPending(cx));

  JS::RootedValueArray<0> args(cx);

  RootedValue rv(cx);
  if (!JS_CallFunctionName(cx, *js::gModuleObject, "ClearPauseData", args, &rv)) {
    MOZ_CRASH("ClearPauseDataCallback");
  }
}

}  // namespace js

///////////////////////////////////////////////////////////////////////////////
// Plumbing
///////////////////////////////////////////////////////////////////////////////

bool DefineRecordReplayControlObject(JSContext* aCx, JS::HandleObject object) {
  MOZ_RELEASE_ASSERT(IsRecordingOrReplaying());

  RootedObject staticObject(aCx, JS_NewObject(aCx, nullptr));
  if (!staticObject ||
      !JS_DefineProperty(aCx, object, "RecordReplayControl", staticObject, 0)) {
    return false;
  }

  if (js::gModuleObject) {
    // RecordReplayControl objects created while setting up the module itself
    // don't get references to the module.
    RootedObject obj(aCx, *js::gModuleObject);
    if (!JS_WrapObject(aCx, &obj) ||
        !JS_DefineProperty(aCx, staticObject, "module", obj, 0)) {
      return false;
    }
  }

  if (!JS_DefineFunctions(aCx, staticObject, js::gRecordReplayMethods)) {
    return false;
  }

  return true;
}

static ProgressCounter gLastRepaintNeededProgress;

// Add annotations to the recording to indicate places where the screen becomes
// dirty. These are currently used to stress test repainting and other DOM
// commands.
void OnRepaintNeeded(const char* aWhy) {
  if (!HasCheckpoint() || HasDivergedFromRecording() || !NS_IsMainThread()) {
    return;
  }

  // Ignore repaints triggered when there hasn't been any execution since the
  // last repaint was triggered.
  if (*ExecutionProgressCounter() == gLastRepaintNeededProgress) {
    return;
  }

  nsPrintfCString contents("{\"why\":\"%s\"}", aWhy);
  js::gOnAnnotation("repaint-needed", contents.get());

  // Measure this after calling RecordReplayOnAnnotation, as the latter can
  // update the progress counter.
  gLastRepaintNeededProgress = *ExecutionProgressCounter();
}

void OnTestCommand(const char* aString) {
  // Ignore commands to finish the current test if we aren't recording/replaying.
  if (!strcmp(aString, "RecReplaySendAsyncMessage Example__Finished") &&
      !IsRecordingOrReplaying()) {
    return;
  }

  js::EnsureModuleInitialized();

  AutoSafeJSContext cx;
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());

  JSString* str = JS_NewStringCopyZ(cx, aString);
  MOZ_RELEASE_ASSERT(str);

  JS::RootedValueArray<1> args(cx);
  args[0].setString(str);

  RootedValue rv(cx);
  if (!JS_CallFunctionName(cx, *js::gModuleObject, "OnTestCommand", args, &rv)) {
    MOZ_CRASH("OnTestCommand");
  }
}

bool BuildJSON(size_t aNumProperties,
               const char** aPropertyNames, const char** aPropertyValues,
               void* aResultRaw) {
  nsCString* result = (nsCString*)aResultRaw;

  AutoSafeJSContext cx;
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());

  RootedObject obj(cx, JS_NewObject(cx, nullptr));
  if (!obj) {
    return false;
  }

  for (size_t i = 0; i < aNumProperties; i++) {
    RootedString valueStr(cx, JS_NewStringCopyZ(cx, aPropertyValues[i]));
    RootedValue value(cx, StringValue(valueStr));
    if (!valueStr || !JS_DefineProperty(cx, obj, aPropertyNames[i], value, JSPROP_ENUMERATE)) {
      return false;
    }
  }

  return JS::ToJSONMaybeSafely(cx, obj, js::FillStringCallback, result);
}

}  // namespace recordreplay
}  // namespace mozilla
