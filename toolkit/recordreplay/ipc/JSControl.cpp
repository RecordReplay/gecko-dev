/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "JSControl.h"

#include "mozilla/Base64.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/StaticPtr.h"
#include "js/CharacterEncoding.h"
#include "js/Conversions.h"
#include "js/JSON.h"
#include "js/PropertySpec.h"
#include "ChildInternal.h"
#include "ParentInternal.h"
#include "nsImportModule.h"
#include "rrIModule.h"
#include "xpcprivate.h"

#include "nsMediaFeatures.h"

using namespace JS;

namespace mozilla {
namespace recordreplay {
namespace js {

// Callback for filling CharBuffers when converting objects to JSON.
static bool FillCharBufferCallback(const char16_t* buf, uint32_t len,
                                   void* data) {
  CharBuffer* buffer = (CharBuffer*)data;
  MOZ_RELEASE_ASSERT(buffer->length() == 0);
  buffer->append(buf, len);
  return true;
}

static JSObject* RequireObject(JSContext* aCx, HandleValue aValue) {
  if (!aValue.isObject()) {
    JS_ReportErrorASCII(aCx, "Expected object");
    return nullptr;
  }
  return &aValue.toObject();
}

static bool RequireNumber(JSContext* aCx, HandleValue aValue, size_t* aNumber) {
  if (!aValue.isNumber()) {
    JS_ReportErrorASCII(aCx, "Expected number");
    return false;
  }
  *aNumber = aValue.toNumber();
  return true;
}

static parent::ChildProcessInfo* ToChildProcess(JSContext* aCx,
                                                HandleValue aRootValue,
                                                HandleValue aForkValue,
                                                size_t* aForkId) {
  size_t rootId;
  if (!RequireNumber(aCx, aRootValue, &rootId) ||
      !RequireNumber(aCx, aForkValue, aForkId)) {
    return nullptr;
  }
  parent::ChildProcessInfo* child = parent::GetChildProcess(rootId);
  if (!child) {
    JS_ReportErrorASCII(aCx, "Bad child ID");
    return nullptr;
  }
  return child;
}

static parent::ChildProcessInfo* ToChildProcess(JSContext* aCx,
                                                HandleValue aRootValue) {
  RootedValue forkValue(aCx, Int32Value(0));
  size_t forkId;
  return ToChildProcess(aCx, aRootValue, forkValue, &forkId);
}

static void InitializeScriptHits();

static nsCString gModuleText;

void SetWebReplayJS(const nsCString& aModule) {
  MOZ_RELEASE_ASSERT(gModuleText.IsEmpty());
  gModuleText = aModule;
}

// URL of the root module script.
#define ModuleURL "resource://devtools/server/actors/replay/module.js"

static StaticRefPtr<rrIModule> gModule;
static PersistentRootedObject* gModuleObject;

bool IsInitialized() {
  return !!gModule;
}

static void EnsureInitialized() {
  if (IsInitialized()) {
    return;
  }
  MOZ_RELEASE_ASSERT(!gModuleText.IsEmpty());

  // Initialization so we can repaint at the first checkpoint without having
  // an unhandled recording divergence.
  nsMediaFeatures::InitSystemMetrics();

  AutoSafeJSContext cx;
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());

  nsCOMPtr<rrIModule> module = do_ImportModule(ModuleURL);
  gModule = module.forget();
  ClearOnShutdown(&gModule);

  RootedValue value(cx);
  if (NS_FAILED(gModule->Initialize(gModuleText, &value))) {
    MOZ_CRASH("SetupModule: Initialize failed");
  }
  MOZ_RELEASE_ASSERT(value.isObject());

  gModuleObject = new PersistentRootedObject(cx);
  *gModuleObject = &value.toObject();

  if (IsRecordingOrReplaying()) {
    InitializeScriptHits();
  }
}

///////////////////////////////////////////////////////////////////////////////
// Middleman Control
///////////////////////////////////////////////////////////////////////////////

void SetupMiddlemanControl(const Maybe<size_t>& aRecordingChildId) {
  EnsureInitialized();

  AutoSafeJSContext cx;
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());

  RootedValue rv(cx);
  RootedValue recordingChildValue(cx);
  if (aRecordingChildId.isSome()) {
    recordingChildValue.setInt32(aRecordingChildId.ref());
  }
  HandleValueArray args(recordingChildValue);
  if (!JS_CallFunctionName(cx, *gModuleObject, "Initialize", args, &rv)) {
    MOZ_CRASH("SetupMiddlemanControl");
  }
}

static void ForwardManifestFinished(parent::ChildProcessInfo* aChild,
                                    size_t aForkId, const char* aBuffer,
                                    size_t aBufferSize) {
  MOZ_RELEASE_ASSERT(IsInitialized());

  AutoSafeJSContext cx;
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());

  JS::AutoValueArray<3> args(cx);
  args[0].setInt32(aChild->GetId());
  args[1].setInt32(aForkId);

  NS_ConvertUTF8toUTF16 buf(aBuffer, aBufferSize);

  if (aBufferSize && !JS_ParseJSON(cx, buf.get(), buf.Length(), args[2])) {
    MOZ_CRASH("ForwardManifestFinished");
  }

  RootedValue rv(cx);
  if (!JS_CallFunctionName(cx, *gModuleObject, "ManifestFinished", args, &rv)) {
    MOZ_CRASH("ForwardManifestFinished");
  }
}

void ForwardManifestFinished(parent::ChildProcessInfo* aChild,
                             const ManifestFinishedMessage& aMsg) {
  ForwardManifestFinished(aChild, aMsg.mForkId, aMsg.BinaryData(),
                          aMsg.BinaryDataSize());
}

void ForwardUnhandledDivergence(parent::ChildProcessInfo* aChild,
                                const UnhandledDivergenceMessage& aMsg) {
  char buf[] = "{\"unhandledDivergence\":true}";
  ForwardManifestFinished(aChild, aMsg.mForkId, buf, sizeof(buf) - 1);
}

void ForwardPingResponse(parent::ChildProcessInfo* aChild,
                         const PingResponseMessage& aMsg) {
  MOZ_RELEASE_ASSERT(IsInitialized());

  AutoSafeJSContext cx;
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());

  JS::AutoValueArray<4> args(cx);
  args[0].setInt32(aChild->GetId());
  args[1].setInt32(aMsg.mForkId);
  args[2].setNumber(aMsg.mId);
  args[3].setNumber((double)aMsg.mProgress);

  RootedValue rv(cx);
  if (!JS_CallFunctionName(cx, *gModuleObject, "PingResponse", args, &rv)) {
    MOZ_CRASH("ForwardPingResponse");
  }
}

void BeforeSaveRecording() {
  AutoSafeJSContext cx;
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());

  RootedValue rv(cx);
  if (!JS_CallFunctionName(cx, *gModuleObject, "BeforeSaveRecording", HandleValueArray::empty(), &rv)) {
    MOZ_CRASH("BeforeSaveRecording");
  }
}

void AfterSaveRecording() {
  AutoSafeJSContext cx;
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());

  RootedValue rv(cx);
  if (!JS_CallFunctionName(cx, *gModuleObject, "AfterSaveRecording", HandleValueArray::empty(), &rv)) {
    MOZ_CRASH("AfterSaveRecording");
  }
}

void SaveCloudRecording(const nsAString& aUUID) {
  AutoSafeJSContext cx;
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());

  JS::RootedValue arg(cx, JS::StringValue(ConvertStringToJSString(cx, aUUID)));
  RootedValue rv(cx);
  if (!JS_CallFunctionName(cx, *gModuleObject, "SaveCloudRecording", HandleValueArray(arg), &rv)) {
    MOZ_CRASH("SaveCloudRecording");
  }
}

bool RecoverFromCrash(size_t aRootId, size_t aForkId) {
  AutoSafeJSContext cx;
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());

  JS::AutoValueArray<2> args(cx);
  args[0].setInt32(aRootId);
  args[1].setInt32(aForkId);

  RootedValue rv(cx);
  return JS_CallFunctionName(cx, *gModuleObject, "RecoverFromCrash", args, &rv);
}

void OnCriticalError(const char* aWhy) {
  AutoSafeJSContext cx;
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());

  JSString* str = JS_NewStringCopyZ(cx, aWhy);
  MOZ_RELEASE_ASSERT(str);

  RootedValue rv(cx);
  RootedValue arg(cx, StringValue(str));
  if (!JS_CallFunctionName(cx, *gModuleObject, "OnCriticalError", HandleValueArray(arg), &rv)) {
    MOZ_CRASH("OnCriticalError");
  }
}

void PaintComplete() {
  AutoSafeJSContext cx;
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());

  RootedValue rv(cx);
  if (!JS_CallFunctionName(cx, *gModuleObject, "PaintComplete", HandleValueArray::empty(), &rv)) {
    MOZ_CRASH("PaintComplete");
  }
}

void OnMouseEvent(const TimeDuration& aTime, const char* aType, int32_t aX, int32_t aY) {
  if (!IsInitialized()) {
    return;
  }

  AutoSafeJSContext cx;
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());

  JSString* str = JS_AtomizeString(cx, aType);
  if (!str) {
    MOZ_CRASH("OnMouseEvent");
  }

  JS::AutoValueArray<4> args(cx);
  args[0].setInt32(aTime.ToMilliseconds());
  args[1].setString(str);
  args[2].setInt32(aX);
  args[3].setInt32(aY);

  RootedValue rv(cx);
  if (!JS_CallFunctionName(cx, *gModuleObject, "OnMouseEvent", args, &rv)) {
    MOZ_CRASH("OnMouseEvent");
  }
}

///////////////////////////////////////////////////////////////////////////////
// Middleman Methods
///////////////////////////////////////////////////////////////////////////////

static bool Middleman_SpawnReplayingChild(JSContext* aCx, unsigned aArgc,
                                          Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  if (!args.get(0).isNumber()) {
    JS_ReportErrorASCII(aCx, "Expected numeric argument");
    return false;
  }

  size_t id = args.get(0).toNumber();
  parent::SpawnReplayingChild(id);
  args.rval().setUndefined();
  return true;
}

static bool Middleman_SendManifest(JSContext* aCx, unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  RootedObject manifestObject(aCx, RequireObject(aCx, args.get(2)));
  if (!manifestObject) {
    return false;
  }

  CharBuffer manifestBuffer;
  if (!ToJSONMaybeSafely(aCx, manifestObject, FillCharBufferCallback,
                         &manifestBuffer)) {
    return false;
  }

  size_t forkId;
  parent::ChildProcessInfo* child = ToChildProcess(aCx, args.get(0),
                                                   args.get(1), &forkId);
  if (!child) {
    return false;
  }

  NS_ConvertUTF16toUTF8 buf(manifestBuffer.begin(), manifestBuffer.length());

  ManifestStartMessage* msg = ManifestStartMessage::New(
      forkId, 0, buf.get(), buf.Length());
  child->SendMessage(std::move(*msg));
  free(msg);

  args.rval().setUndefined();
  return true;
}

static bool Middleman_Ping(JSContext* aCx, unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  size_t forkId;
  parent::ChildProcessInfo* child = ToChildProcess(aCx, args.get(0),
                                                   args.get(1), &forkId);
  if (!child) {
    return false;
  }

  size_t pingId;
  if (!RequireNumber(aCx, args.get(2), &pingId)) {
    return false;
  }

  child->SendMessage(PingMessage(forkId, pingId));

  args.rval().setUndefined();
  return true;
}

static bool Middleman_HadRepaint(JSContext* aCx, unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  if (!args.get(0).isString()) {
    parent::ClearGraphics();

    args.rval().setUndefined();
    return true;
  }

  nsAutoCString dataCString;
  ConvertJSStringToCString(aCx, args.get(0).toString(), dataCString);

  nsCString dataBinary;
  nsresult rv = Base64Decode(dataCString, dataBinary);

  if (NS_FAILED(rv)) {
    JS_ReportErrorASCII(aCx, "Base64 decode failed");
    return false;
  }

  int cursorX = -1, cursorY = -1;
  if (args.get(1).isNumber() && args.get(2).isNumber()) {
    cursorX = args.get(1).toNumber();
    cursorY = args.get(2).toNumber();
  }

  int clickX = -1, clickY = -1;
  if (args.get(3).isNumber() && args.get(4).isNumber()) {
    clickX = args.get(3).toNumber();
    clickY = args.get(4).toNumber();
  }

  parent::UpdateGraphicsAfterRepaint(dataBinary, cursorX, cursorY, clickX, clickY);

  args.rval().setUndefined();
  return true;
}

static bool Middleman_RestoreMainGraphics(JSContext* aCx, unsigned aArgc,
                                          Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  parent::RestoreMainGraphics();

  args.rval().setUndefined();
  return true;
}

static bool Middleman_ClearGraphics(JSContext* aCx, unsigned aArgc,
                                    Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  parent::ClearGraphics();

  args.rval().setUndefined();
  return true;
}

static bool Middleman_InRepaintStressMode(JSContext* aCx, unsigned aArgc,
                                          Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  args.rval().setBoolean(parent::InRepaintStressMode());
  return true;
}

static bool Middleman_CreateCheckpointInRecording(JSContext* aCx,
                                                  unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  parent::ChildProcessInfo* child = ToChildProcess(aCx, args.get(0));
  if (!child) {
    return false;
  }

  if (!child->IsRecording()) {
    JS_ReportErrorASCII(aCx, "Need recording child");
    return false;
  }

  // Recording children can idle indefinitely while waiting for input, without
  // creating a checkpoint. If this might be a problem, this method induces the
  // child to create a new checkpoint and pause.
  child->SendMessage(CreateCheckpointMessage());

  args.rval().setUndefined();
  return true;
}

static bool Middleman_MaybeProcessNextMessage(JSContext* aCx, unsigned aArgc,
                                              Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  parent::ChildProcessInfo::MaybeProcessNextMessage();

  args.rval().setUndefined();
  return true;
}

static bool Middleman_Terminate(JSContext* aCx, unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  size_t forkId;
  parent::ChildProcessInfo* child = ToChildProcess(aCx, args.get(0),
                                                   args.get(1), &forkId);
  if (!child) {
    return false;
  }

  child->SendMessage(TerminateMessage(forkId));

  args.rval().setUndefined();
  return true;
}

static bool Middleman_CrashHangedChild(JSContext* aCx, unsigned aArgc,
                                       Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  size_t forkId;
  parent::ChildProcessInfo* child = ToChildProcess(aCx, args.get(0),
                                                   args.get(1), &forkId);
  if (!child) {
    return false;
  }

  // Try to get the child to crash, so that we can get a minidump.
  CrashReporter::AnnotateCrashReport(
      CrashReporter::Annotation::RecordReplayHang, true);
  child->SendMessage(CrashMessage(forkId));

  args.rval().setUndefined();
  return true;
}

static bool Middleman_RecordingLength(JSContext* aCx, unsigned aArgc,
                                      Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);
  args.rval().setNumber((double)parent::gRecordingContents.length());
  return true;
}

static bool Middleman_UpdateRecording(JSContext* aCx, unsigned aArgc,
                                      Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  parent::ChildProcessInfo* child = ToChildProcess(aCx, args.get(0));
  if (!child) {
    return false;
  }

  if (!args.get(1).isNumber() || !args.get(2).isNumber()) {
    JS_ReportErrorASCII(aCx, "Expected numeric argument");
    return false;
  }

  size_t start = args.get(1).toNumber();
  size_t size = args.get(2).toNumber();
  MOZ_RELEASE_ASSERT(start + size <= parent::gRecordingContents.length());

  UniquePtr<Message> msg(RecordingDataMessage::New(
      0, start, parent::gRecordingContents.begin() + start, size));
  child->SendMessage(std::move(*msg));

  args.rval().setUndefined();
  return true;
}

static bool Middleman_SetActiveChildIsRecording(JSContext* aCx, unsigned aArgc,
                                                Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  parent::gActiveChildIsRecording = ToBoolean(args.get(0));

  args.rval().setUndefined();
  return true;
}

///////////////////////////////////////////////////////////////////////////////
// Devtools Sandbox
///////////////////////////////////////////////////////////////////////////////

extern "C" {

MOZ_EXPORT bool RecordReplayInterface_ShouldUpdateProgressCounter(
    const char* aURL) {
  // Progress counters are only updated for scripts which are exposed to the
  // debugger. The devtools timeline is based on progress values and we don't
  // want gaps on the timeline which users can't seek to.
  return aURL && strncmp(aURL, "resource:", 9) && strncmp(aURL, "chrome:", 7);
}

}  // extern "C"

#undef ReplayScriptURL

void ManifestStart(const CharBuffer& aContents) {
  AutoDisallowThreadEvents disallow;
  AutoSafeJSContext cx;
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());

  RootedValue value(cx);
  if (!JS_ParseJSON(cx, aContents.begin(), aContents.length(), &value)) {
    MOZ_CRASH("ManifestStart: ParseJSON failed");
  }

  RootedValue rv(cx);
  HandleValueArray args(value);
  if (!JS_CallFunctionName(cx, *gModuleObject, "ManifestStart", args, &rv)) {
    MOZ_CRASH("ManifestStart: Handler failed");
  }

  // Processing the manifest may have called into MaybeDivergeFromRecording.
  // If it did so, we should already have finished any processing that required
  // diverging from the recording. Don't tolerate future events that
  // would otherwise cause us to rewind to the last checkpoint.
  DisallowUnhandledDivergeFromRecording();
}

void HitCheckpoint(size_t aCheckpoint, TimeDuration aTime) {
  EnsureInitialized();

  AutoDisallowThreadEvents disallow;
  AutoSafeJSContext cx;
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());

  RootedValue rv(cx);
  JS::AutoValueArray<2> args(cx);
  args[0].setInt32(aCheckpoint);
  args[1].setInt32(aTime.ToMilliseconds());
  if (!JS_CallFunctionName(cx, *gModuleObject, "HitCheckpoint", args, &rv)) {
    MOZ_CRASH("HitCheckpoint");
  }
}

bool CanCreateCheckpoint() {
  if (!IsInitialized()) {
    return true;
  }

  AutoSafeJSContext cx;
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());

  RootedValue rv(cx);
  if (!JS_CallFunctionName(cx, *gModuleObject, "CanCreateCheckpoint",
                           JS::HandleValueArray::empty(), &rv)) {
    MOZ_CRASH("CanCreateCheckpoint");
  }

  return ToBoolean(rv);
}

static ProgressCounter gProgressCounter;

extern "C" {

MOZ_EXPORT ProgressCounter* RecordReplayInterface_ExecutionProgressCounter() {
  return &gProgressCounter;
}

MOZ_EXPORT ProgressCounter RecordReplayInterface_NewTimeWarpTarget() {
  if (AreThreadEventsDisallowed()) {
    return 0;
  }

  // NewTimeWarpTarget() must be called at consistent points between recording
  // and replaying.
  RecordReplayAssert("NewTimeWarpTarget");

  if (!IsInitialized()) {
    return 0;
  }

  AutoDisallowThreadEvents disallow;
  AutoSafeJSContext cx;
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());

  RootedValue rv(cx);
  if (!JS_CallFunctionName(cx, *gModuleObject, "NewTimeWarpTarget", HandleValueArray::empty(), &rv)) {
    MOZ_CRASH("NewTimeWarpTarget");
  }

  MOZ_RELEASE_ASSERT(rv.isNumber());
  return rv.toNumber();
}

}  // extern "C"

///////////////////////////////////////////////////////////////////////////////
// Replaying process content
///////////////////////////////////////////////////////////////////////////////

struct ContentInfo {
  const void* mToken;
  char* mURL;
  char* mContentType;
  InfallibleVector<char> mContent8;
  InfallibleVector<char16_t> mContent16;

  ContentInfo(const void* aToken, const char* aURL, const char* aContentType)
      : mToken(aToken),
        mURL(strdup(aURL)),
        mContentType(strdup(aContentType)) {}

  ContentInfo(ContentInfo&& aOther)
      : mToken(aOther.mToken),
        mURL(aOther.mURL),
        mContentType(aOther.mContentType),
        mContent8(std::move(aOther.mContent8)),
        mContent16(std::move(aOther.mContent16)) {
    aOther.mURL = nullptr;
    aOther.mContentType = nullptr;
  }

  ~ContentInfo() {
    free(mURL);
    free(mContentType);
  }

  size_t Length() {
    MOZ_RELEASE_ASSERT(!mContent8.length() || !mContent16.length());
    return mContent8.length() ? mContent8.length() : mContent16.length();
  }
};

// All content that has been parsed so far. Protected by child::gMonitor.
static StaticInfallibleVector<ContentInfo> gContent;

extern "C" {

MOZ_EXPORT void RecordReplayInterface_BeginContentParse(
    const void* aToken, const char* aURL, const char* aContentType) {
  MOZ_RELEASE_ASSERT(IsRecordingOrReplaying());
  MOZ_RELEASE_ASSERT(aToken);

  MonitorAutoLock lock(*child::gMonitor);
  for (ContentInfo& info : gContent) {
    MOZ_RELEASE_ASSERT(info.mToken != aToken);
  }
  gContent.emplaceBack(aToken, aURL, aContentType);
}

MOZ_EXPORT void RecordReplayInterface_AddContentParseData8(
    const void* aToken, const Utf8Unit* aUtf8Buffer, size_t aLength) {
  MOZ_RELEASE_ASSERT(IsRecordingOrReplaying());
  MOZ_RELEASE_ASSERT(aToken);

  MonitorAutoLock lock(*child::gMonitor);
  for (ContentInfo& info : gContent) {
    if (info.mToken == aToken) {
      info.mContent8.append(reinterpret_cast<const char*>(aUtf8Buffer),
                            aLength);
      return;
    }
  }
  MOZ_CRASH("Unknown content parse token");
}

MOZ_EXPORT void RecordReplayInterface_AddContentParseData16(
    const void* aToken, const char16_t* aBuffer, size_t aLength) {
  MOZ_RELEASE_ASSERT(IsRecordingOrReplaying());
  MOZ_RELEASE_ASSERT(aToken);

  MonitorAutoLock lock(*child::gMonitor);
  for (ContentInfo& info : gContent) {
    if (info.mToken == aToken) {
      info.mContent16.append(aBuffer, aLength);
      return;
    }
  }
  MOZ_CRASH("Unknown content parse token");
}

MOZ_EXPORT void RecordReplayInterface_EndContentParse(const void* aToken) {
  MOZ_RELEASE_ASSERT(IsRecordingOrReplaying());
  MOZ_RELEASE_ASSERT(aToken);

  MonitorAutoLock lock(*child::gMonitor);
  for (ContentInfo& info : gContent) {
    if (info.mToken == aToken) {
      info.mToken = nullptr;
      return;
    }
  }
  MOZ_CRASH("Unknown content parse token");
}

}  // extern "C"

static bool FetchContent(JSContext* aCx, HandleString aURL,
                         MutableHandleString aContentType,
                         MutableHandleString aContent) {
  MonitorAutoLock lock(*child::gMonitor);

  // Find the longest content parse data with this URL. This is to handle inline
  // script elements in HTML pages, where we will see content parses for both
  // the HTML itself and for each inline script.
  ContentInfo* best = nullptr;
  for (ContentInfo& info : gContent) {
    if (JS_LinearStringEqualsAscii(JS_ASSERT_STRING_IS_LINEAR(aURL),
                                   info.mURL)) {
      if (!best || info.Length() > best->Length()) {
        best = &info;
      }
    }
  }

  if (best) {
    aContentType.set(JS_NewStringCopyZ(aCx, best->mContentType));

    MOZ_ASSERT(best->mContent8.length() == 0 || best->mContent16.length() == 0,
               "should have content data of only one type");

    aContent.set(best->mContent8.length() > 0
                     ? JS_NewStringCopyUTF8N(
                           aCx, JS::UTF8Chars(best->mContent8.begin(),
                                              best->mContent8.length()))
                     : JS_NewUCStringCopyN(aCx, best->mContent16.begin(),
                                           best->mContent16.length()));
  } else {
    aContentType.set(JS_NewStringCopyZ(aCx, "text/plain"));
    aContent.set(
        JS_NewStringCopyZ(aCx, "Could not find record/replay content"));
  }

  return aContentType && aContent;
}

///////////////////////////////////////////////////////////////////////////////
// Recording/Replaying Methods
///////////////////////////////////////////////////////////////////////////////

static bool RecordReplay_Fork(JSContext* aCx, unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  if (!args.get(0).isNumber()) {
    JS_ReportErrorASCII(aCx, "Expected numeric argument");
    return false;
  }

  size_t id = args.get(0).toNumber();
  child::PerformFork(id);

  args.rval().setUndefined();
  return true;
}

static bool RecordReplay_ChildId(JSContext* aCx, unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  args.rval().setInt32(child::GetId());
  return true;
}

static bool RecordReplay_ForkId(JSContext* aCx, unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  args.rval().setInt32(child::GetForkId());
  return true;
}

static bool RecordReplay_EnsureRecordingLength(JSContext* aCx, unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  if (!args.get(0).isNumber()) {
    JS_ReportErrorASCII(aCx, "Expected numeric argument");
    return false;
  }

  size_t length = args.get(0).toNumber();
  child::EnsureRecordingLength(length);

  args.rval().setUndefined();
  return true;
}

static bool RecordReplay_AreThreadEventsDisallowed(JSContext* aCx,
                                                   unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);
  args.rval().setBoolean(AreThreadEventsDisallowed());
  return true;
}

static bool RecordReplay_DivergeFromRecording(JSContext* aCx, unsigned aArgc,
                                              Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);
  DivergeFromRecording();
  args.rval().setUndefined();
  return true;
}

static bool RecordReplay_ProgressCounter(JSContext* aCx, unsigned aArgc,
                                         Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);
  args.rval().setNumber((double)gProgressCounter);
  return true;
}

static bool RecordReplay_SetProgressCounter(JSContext* aCx, unsigned aArgc,
                                            Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  if (!args.get(0).isNumber()) {
    JS_ReportErrorASCII(aCx, "Expected numeric argument");
    return false;
  }

  gProgressCounter = args.get(0).toNumber();

  args.rval().setUndefined();
  return true;
}

JSString* ConvertStringToJSString(JSContext* aCx, const nsAString& aString) {
  JSString* rv = JS_NewUCStringCopyN(aCx, aString.BeginReading(), aString.Length());
  MOZ_RELEASE_ASSERT(rv);
  return rv;
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

static bool RecordReplay_ShouldUpdateProgressCounter(JSContext* aCx,
                                                     unsigned aArgc,
                                                     Value* aVp) {
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

static bool RecordReplay_ManifestFinished(JSContext* aCx, unsigned aArgc,
                                          Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  CharBuffer responseBuffer;
  if (args.hasDefined(0)) {
    RootedObject responseObject(aCx, RequireObject(aCx, args.get(0)));
    if (!responseObject) {
      return false;
    }

    if (!ToJSONMaybeSafely(aCx, responseObject, FillCharBufferCallback,
                           &responseBuffer)) {
      return false;
    }
  }

  child::ManifestFinished(responseBuffer);

  args.rval().setUndefined();
  return true;
}

static bool RecordReplay_ResumeExecution(JSContext* aCx, unsigned aArgc,
                                         Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  ResumeExecution();

  args.rval().setUndefined();
  return true;
}

// The total amount of time this process has spent idling.
static double gIdleTimeTotal;

// When recording and we are idle, the time when we became idle.
static double gIdleTimeStart;

void BeginIdleTime() {
  if (IsRecording() && Thread::CurrentIsMainThread()) {
    MOZ_RELEASE_ASSERT(!gIdleTimeStart);
    gIdleTimeStart = CurrentTime();
  }
}

void EndIdleTime() {
  if (IsRecording() && Thread::CurrentIsMainThread()) {
    MOZ_RELEASE_ASSERT(!!gIdleTimeStart);
    gIdleTimeTotal += CurrentTime() - gIdleTimeStart;
    gIdleTimeStart = 0;
  }
}

static bool RecordReplay_CurrentExecutionTime(JSContext* aCx, unsigned aArgc,
                                              Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  // Get a current timestamp biased by the amount of time the process has spent
  // idling. Comparing these timestamps gives the elapsed non-idle time between
  // them.
  args.rval().setNumber((CurrentTime() - gIdleTimeTotal) / 1000.0);
  return true;
}

static bool RecordReplay_FlushRecording(JSContext* aCx, unsigned aArgc,
                                        Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  FlushRecording();

  args.rval().setUndefined();
  return true;
}

static bool RecordReplay_FlushExternalCalls(JSContext* aCx, unsigned aArgc,
                                            Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  FlushExternalCalls();

  args.rval().setUndefined();
  return true;
}

static bool RecordReplay_SetRecordingSummary(JSContext* aCx, unsigned aArgc,
                                             Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  if (!args.get(0).isString()) {
    JS_ReportErrorASCII(aCx, "Expected string argument");
    return false;
  }

  nsAutoCString str;
  ConvertJSStringToCString(aCx, args.get(0).toString(), str);
  SetRecordingSummary(str);

  args.rval().setUndefined();
  return true;
}

static bool RecordReplay_GetRecordingSummary(JSContext* aCx, unsigned aArgc,
                                             Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  nsAutoCString summary;
  GetRecordingSummary(summary);

  JSString* str = JS_NewStringCopyZ(aCx, summary.get());
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

static bool RecordReplay_GetContent(JSContext* aCx, unsigned aArgc,
                                    Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);
  RootedString url(aCx, ToString(aCx, args.get(0)));

  RootedString contentType(aCx), content(aCx);
  if (!FetchContent(aCx, url, &contentType, &content)) {
    return false;
  }

  RootedObject obj(aCx, JS_NewObject(aCx, nullptr));
  if (!obj ||
      !JS_DefineProperty(aCx, obj, "contentType", contentType,
                         JSPROP_ENUMERATE) ||
      !JS_DefineProperty(aCx, obj, "content", content, JSPROP_ENUMERATE)) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

static bool RecordReplay_GetGraphics(JSContext* aCx, unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  nsCString data;
  if (!child::GetGraphics(ToBoolean(args.get(0)), data)) {
    args.rval().setNull();
    return true;
  }

  JSString* str = JS_NewStringCopyN(aCx, data.BeginReading(), data.Length());
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

static bool RecordReplay_GetEnv(JSContext* aCx, unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);
  args.rval().setUndefined();

  if (!args.get(0).isString()) {
    JS_ReportErrorASCII(aCx, "Expected string argument");
    return false;
  }

  if (ReplayingInCloud()) {
    AutoEnsurePassThroughThreadEvents pt;

    nsAutoCString env;
    ConvertJSStringToCString(aCx, args.get(0).toString(), env);

    const char* value = getenv(env.get());
    if (value) {
      JSString* str = JS_NewStringCopyZ(aCx, value);
      if (!str) {
        return false;
      }

      args.rval().setString(str);
    }
  }

  return true;
}

static bool RecordReplay_SaveCloudRecording(JSContext* aCx, unsigned aArgc,
                                            Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  if (!args.get(0).isString()) {
    JS_ReportErrorASCII(aCx, "Expected string argument");
    return false;
  }

  nsAutoCString str;
  ConvertJSStringToCString(aCx, args.get(0).toString(), str);
  child::SaveCloudRecording(str.get());

  args.rval().setUndefined();
  return true;
}

static bool RecordReplay_SetUnhandledDivergenceAllowed(JSContext* aCx,
                                                       unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  child::SetUnhandledDivergenceAllowed(ToBoolean(args.get(0)));

  args.rval().setUndefined();
  return true;
}

static bool RecordReplay_SetCrashNote(JSContext* aCx, unsigned aArgc, Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  if (!args.get(0).isString()) {
    JS_ReportErrorASCII(aCx, "Expected string argument");
    return false;
  }

  nsAutoCString str;
  ConvertJSStringToCString(aCx, args.get(0).toString(), str);
  child::SetCrashNote(str.get());

  args.rval().setUndefined();
  return true;
}

static bool RecordReplay_Dump(JSContext* aCx, unsigned aArgc, Value* aVp) {
  // This method is an alternative to dump() that can be used in places where
  // thread events are disallowed.
  CallArgs args = CallArgsFromVp(aArgc, aVp);
  for (size_t i = 0; i < args.length(); i++) {
    RootedString str(aCx, ToString(aCx, args[i]));
    if (!str) {
      return false;
    }
    JS::UniqueChars cstr = JS_EncodeStringToLatin1(aCx, str);
    if (!cstr) {
      return false;
    }
    DirectPrint(cstr.get());
  }

  args.rval().setUndefined();
  return true;
}

static bool RecordReplay_Crash(JSContext* aCx, unsigned aArgc, Value* aVp) {
  MOZ_CRASH("Intentional Crash");
}

///////////////////////////////////////////////////////////////////////////////
// Recording/Replaying Script Hit Methods
///////////////////////////////////////////////////////////////////////////////

enum ChangeFrameKind {
  ChangeFrameEnter,
  ChangeFrameExit,
  ChangeFrameResume,
  NumChangeFrameKinds
};

struct ScriptHitInfo {
  // Information about a location where a script offset has been hit.
  struct ScriptHit {
    uint32_t mFrameIndex : 16;
    ProgressCounter mProgress : 48;

    ScriptHit(uint32_t aFrameIndex, ProgressCounter aProgress)
        : mFrameIndex(aFrameIndex), mProgress(aProgress) {
      MOZ_RELEASE_ASSERT(aFrameIndex < 1 << 16);
      MOZ_RELEASE_ASSERT(aProgress < uint64_t(1) << 48);
    }
  };

  static_assert(sizeof(ScriptHit) == 8, "Unexpected size");

  typedef InfallibleVector<ScriptHit> ScriptHitVector;

  struct ScriptHitKey {
    uint32_t mScript;
    uint32_t mOffset;

    ScriptHitKey(uint32_t aScript, uint32_t aOffset)
        : mScript(aScript), mOffset(aOffset) {}

    typedef ScriptHitKey Lookup;

    static HashNumber hash(const ScriptHitKey& aKey) {
      return HashGeneric(aKey.mScript, aKey.mOffset);
    }

    static bool match(const ScriptHitKey& aFirst, const ScriptHitKey& aSecond) {
      return aFirst.mScript == aSecond.mScript &&
             aFirst.mOffset == aSecond.mOffset;
    }
  };

  typedef HashMap<ScriptHitKey, ScriptHitVector*, ScriptHitKey>
      ScriptHitMap;

  struct AnyScriptHit {
    uint32_t mScript;
    uint32_t mFrameIndex;
    ProgressCounter mProgress;

    AnyScriptHit(uint32_t aScript, uint32_t aFrameIndex,
                 ProgressCounter aProgress)
        : mScript(aScript), mFrameIndex(aFrameIndex), mProgress(aProgress) {}
  };

  typedef InfallibleVector<AnyScriptHit, 128> AnyScriptHitVector;

  struct CheckpointInfo {
    ScriptHitMap mTable;
    AnyScriptHitVector mChangeFrames[NumChangeFrameKinds];
  };

  InfallibleVector<CheckpointInfo*, 1024> mInfo;

  CheckpointInfo* GetInfo(uint32_t aCheckpoint) {
    while (aCheckpoint >= mInfo.length()) {
      mInfo.append(nullptr);
    }
    if (!mInfo[aCheckpoint]) {
      mInfo[aCheckpoint] = new CheckpointInfo();
    }
    return mInfo[aCheckpoint];
  }

  ScriptHitVector* FindHits(uint32_t aCheckpoint, uint32_t aScript,
                           uint32_t aOffset) {
    CheckpointInfo* info = GetInfo(aCheckpoint);

    ScriptHitKey key(aScript, aOffset);
    ScriptHitMap::Ptr p = info->mTable.lookup(key);
    return p ? p->value() : nullptr;
  }

  void AddHit(uint32_t aCheckpoint, uint32_t aScript, uint32_t aOffset,
              uint32_t aFrameIndex, ProgressCounter aProgress) {
    CheckpointInfo* info = GetInfo(aCheckpoint);

    ScriptHitKey key(aScript, aOffset);
    ScriptHitMap::AddPtr p = info->mTable.lookupForAdd(key);
    if (!p && !info->mTable.add(p, key, new ScriptHitVector())) {
      MOZ_CRASH("ScriptHitInfo::AddHit");
    }

    ScriptHitVector* hits = p->value();
    hits->append(ScriptHit(aFrameIndex, aProgress));
  }

  void AddChangeFrame(uint32_t aCheckpoint, uint32_t aWhich, uint32_t aScript,
                      uint32_t aFrameIndex, ProgressCounter aProgress) {
    CheckpointInfo* info = GetInfo(aCheckpoint);
    MOZ_RELEASE_ASSERT(aWhich < NumChangeFrameKinds);
    info->mChangeFrames[aWhich].emplaceBack(aScript, aFrameIndex, aProgress);
  }

  AnyScriptHitVector* FindChangeFrames(uint32_t aCheckpoint, uint32_t aWhich) {
    CheckpointInfo* info = GetInfo(aCheckpoint);
    MOZ_RELEASE_ASSERT(aWhich < NumChangeFrameKinds);
    return &info->mChangeFrames[aWhich];
  }
};

static ScriptHitInfo* gScriptHits;

// Interned atoms for the various instrumented operations.
static JSString* gMainAtom;
static JSString* gEntryAtom;
static JSString* gBreakpointAtom;
static JSString* gExitAtom;

static void InitializeScriptHits() {
  gScriptHits = new ScriptHitInfo();

  AutoSafeJSContext cx;
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());

  gMainAtom = JS_AtomizeAndPinString(cx, "main");
  gEntryAtom = JS_AtomizeAndPinString(cx, "entry");
  gBreakpointAtom = JS_AtomizeAndPinString(cx, "breakpoint");
  gExitAtom = JS_AtomizeAndPinString(cx, "exit");

  MOZ_RELEASE_ASSERT(gMainAtom && gEntryAtom && gBreakpointAtom && gExitAtom);
}

static bool gScanningScripts;
static uint32_t gFrameDepth;

static bool RecordReplay_IsScanningScripts(JSContext* aCx, unsigned aArgc,
                                           Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  args.rval().setBoolean(gScanningScripts);
  return true;
}

static bool RecordReplay_SetScanningScripts(JSContext* aCx, unsigned aArgc,
                                            Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  MOZ_RELEASE_ASSERT(gFrameDepth == 0);
  gScanningScripts = ToBoolean(args.get(0));

  args.rval().setUndefined();
  return true;
}

static bool RecordReplay_GetFrameDepth(JSContext* aCx, unsigned aArgc,
                                       Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  args.rval().setNumber(gFrameDepth);
  return true;
}

static bool RecordReplay_SetFrameDepth(JSContext* aCx, unsigned aArgc,
                                       Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);
  MOZ_RELEASE_ASSERT(gScanningScripts);

  if (!args.get(0).isNumber()) {
    JS_ReportErrorASCII(aCx, "Bad parameter");
    return false;
  }

  gFrameDepth = args.get(0).toNumber();

  args.rval().setUndefined();
  return true;
}

static bool RecordReplay_OnScriptHit(JSContext* aCx, unsigned aArgc,
                                     Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);
  MOZ_RELEASE_ASSERT(gScanningScripts);

  if (!args.get(1).isNumber() || !args.get(2).isNumber()) {
    JS_ReportErrorASCII(aCx, "Bad parameters");
    return false;
  }

  uint32_t script = args.get(1).toNumber();
  uint32_t offset = args.get(2).toNumber();
  uint32_t frameIndex = gFrameDepth - 1;

  if (!script) {
    // This script is not being tracked and doesn't update the frame depth.
    args.rval().setUndefined();
    return true;
  }

  gScriptHits->AddHit(GetLastCheckpoint(), script, offset, frameIndex,
                      gProgressCounter);
  args.rval().setUndefined();
  return true;
}

template <ChangeFrameKind Kind>
static bool RecordReplay_OnChangeFrame(JSContext* aCx, unsigned aArgc,
                                       Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);
  MOZ_RELEASE_ASSERT(gScanningScripts);

  if (!args.get(1).isNumber()) {
    JS_ReportErrorASCII(aCx, "Bad parameters");
    return false;
  }

  uint32_t script = args.get(1).toNumber();
  if (!script) {
    // This script is not being tracked and doesn't update the frame depth.
    args.rval().setUndefined();
    return true;
  }

  if (Kind == ChangeFrameEnter || Kind == ChangeFrameResume) {
    gFrameDepth++;
  }

  uint32_t frameIndex = gFrameDepth - 1;
  gScriptHits->AddChangeFrame(GetLastCheckpoint(), Kind, script, frameIndex,
                              gProgressCounter);

  if (Kind == ChangeFrameExit) {
    gFrameDepth--;
  }

  args.rval().setUndefined();
  return true;
}

static bool RecordReplay_InstrumentationCallback(JSContext* aCx, unsigned aArgc,
                                                 Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  if (!args.get(0).isString()) {
    JS_ReportErrorASCII(aCx, "Bad parameters");
    return false;
  }

  // The kind string should be an atom which we have captured already.
  JSString* kind = args.get(0).toString();

  if (kind == gBreakpointAtom) {
    return RecordReplay_OnScriptHit(aCx, aArgc, aVp);
  }

  if (kind == gMainAtom) {
    return RecordReplay_OnChangeFrame<ChangeFrameEnter>(aCx, aArgc, aVp);
  }

  if (kind == gExitAtom) {
    return RecordReplay_OnChangeFrame<ChangeFrameExit>(aCx, aArgc, aVp);
  }

  if (kind == gEntryAtom) {
    JSAutoRealm ar(aCx, xpc::PrivilegedJunkScope());

    RootedValue rv(aCx);
    HandleValueArray resumeArgs(args.get(1));
    if (!JS_CallFunctionName(aCx, *gModuleObject, "ScriptResumeFrame", resumeArgs, &rv)) {
      MOZ_CRASH("RecordReplay_InstrumentationCallback");
    }

    args.rval().setUndefined();
    return true;
  }

  JS_ReportErrorASCII(aCx, "Unexpected kind");
  return false;
}

static bool RecordReplay_FindScriptHits(JSContext* aCx, unsigned aArgc,
                                        Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  if (!args.get(0).isNumber() || !args.get(1).isNumber() ||
      !args.get(2).isNumber()) {
    JS_ReportErrorASCII(aCx, "Bad parameters");
    return false;
  }

  uint32_t checkpoint = args.get(0).toNumber();
  uint32_t script = args.get(1).toNumber();
  uint32_t offset = args.get(2).toNumber();

  RootedValueVector values(aCx);

  ScriptHitInfo::ScriptHitVector* hits =
      gScriptHits ? gScriptHits->FindHits(checkpoint, script, offset) : nullptr;
  if (hits) {
    for (const auto& hit : *hits) {
      RootedObject hitObject(aCx, JS_NewObject(aCx, nullptr));
      if (!hitObject ||
          !JS_DefineProperty(aCx, hitObject, "progress",
                             (double)hit.mProgress, JSPROP_ENUMERATE) ||
          !JS_DefineProperty(aCx, hitObject, "frameIndex", hit.mFrameIndex,
                             JSPROP_ENUMERATE) ||
          !values.append(ObjectValue(*hitObject))) {
        return false;
      }
    }
  }

  JSObject* array = JS_NewArrayObject(aCx, values);
  if (!array) {
    return false;
  }

  args.rval().setObject(*array);
  return true;
}

static bool MaybeGetNumberProperty(JSContext* aCx, HandleObject aObject,
                                   const char* aName, Maybe<size_t>* aResult) {
  RootedValue v(aCx);
  if (!JS_GetProperty(aCx, aObject, aName, &v)) {
    return false;
  }

  if (v.isNumber()) {
    aResult->emplace(v.toNumber());
  }

  return true;
}

static bool RecordReplay_FindChangeFrames(JSContext* aCx, unsigned aArgc,
                                          Value* aVp) {
  CallArgs args = CallArgsFromVp(aArgc, aVp);

  if (!args.get(0).isNumber() || !args.get(1).isNumber() ||
      !args.get(2).isObject()) {
    JS_ReportErrorASCII(aCx, "Bad parameters");
    return false;
  }

  uint32_t checkpoint = args.get(0).toNumber();
  uint32_t which = args.get(1).toNumber();

  if (which >= NumChangeFrameKinds) {
    JS_ReportErrorASCII(aCx, "Bad parameters");
    return false;
  }

  Maybe<size_t> frameIndex;
  Maybe<size_t> script;
  Maybe<size_t> minProgress;
  Maybe<size_t> maxProgress;

  RootedObject filter(aCx, &args.get(2).toObject());
  if (!MaybeGetNumberProperty(aCx, filter, "frameIndex", &frameIndex) ||
      !MaybeGetNumberProperty(aCx, filter, "script", &script) ||
      !MaybeGetNumberProperty(aCx, filter, "minProgress", &minProgress) ||
      !MaybeGetNumberProperty(aCx, filter, "maxProgress", &maxProgress)) {
    return false;
  }

  RootedValueVector values(aCx);

  ScriptHitInfo::AnyScriptHitVector* hits =
      gScriptHits ? gScriptHits->FindChangeFrames(checkpoint, which) : nullptr;
  if (hits) {
    for (const ScriptHitInfo::AnyScriptHit& hit : *hits) {
      if ((frameIndex.isSome() && hit.mFrameIndex != *frameIndex) ||
          (script.isSome() && hit.mScript != *script) ||
          (minProgress.isSome() && hit.mProgress < *minProgress) ||
          (maxProgress.isSome() && hit.mProgress > *maxProgress)) {
        continue;
      }
      RootedObject hitObject(aCx, JS_NewObject(aCx, nullptr));
      if (!hitObject ||
          !JS_DefineProperty(aCx, hitObject, "script", hit.mScript,
                             JSPROP_ENUMERATE) ||
          !JS_DefineProperty(aCx, hitObject, "progress", (double)hit.mProgress,
                             JSPROP_ENUMERATE) ||
          !JS_DefineProperty(aCx, hitObject, "frameIndex", hit.mFrameIndex,
                             JSPROP_ENUMERATE) ||
          !values.append(ObjectValue(*hitObject))) {
        return false;
      }
    }
  }

  JSObject* array = JS_NewArrayObject(aCx, values);
  if (!array) {
    return false;
  }

  args.rval().setObject(*array);
  return true;
}

///////////////////////////////////////////////////////////////////////////////
// Plumbing
///////////////////////////////////////////////////////////////////////////////

static const JSFunctionSpec gMiddlemanMethods[] = {
    JS_FN("spawnReplayingChild", Middleman_SpawnReplayingChild, 1, 0),
    JS_FN("sendManifest", Middleman_SendManifest, 3, 0),
    JS_FN("ping", Middleman_Ping, 3, 0),
    JS_FN("hadRepaint", Middleman_HadRepaint, 5, 0),
    JS_FN("restoreMainGraphics", Middleman_RestoreMainGraphics, 0, 0),
    JS_FN("clearGraphics", Middleman_ClearGraphics, 0, 0),
    JS_FN("inRepaintStressMode", Middleman_InRepaintStressMode, 0, 0),
    JS_FN("createCheckpointInRecording", Middleman_CreateCheckpointInRecording,
          1, 0),
    JS_FN("maybeProcessNextMessage", Middleman_MaybeProcessNextMessage, 0, 0),
    JS_FN("terminate", Middleman_Terminate, 2, 0),
    JS_FN("crashHangedChild", Middleman_CrashHangedChild, 2, 0),
    JS_FN("recordingLength", Middleman_RecordingLength, 0, 0),
    JS_FN("updateRecording", Middleman_UpdateRecording, 3, 0),
    JS_FN("setActiveChildIsRecording", Middleman_SetActiveChildIsRecording, 1, 0),
    JS_FS_END};

static const JSFunctionSpec gRecordReplayMethods[] = {
    JS_FN("fork", RecordReplay_Fork, 1, 0),
    JS_FN("childId", RecordReplay_ChildId, 0, 0),
    JS_FN("forkId", RecordReplay_ForkId, 0, 0),
    JS_FN("ensureRecordingLength", RecordReplay_EnsureRecordingLength, 1, 0),
    JS_FN("areThreadEventsDisallowed", RecordReplay_AreThreadEventsDisallowed,
          0, 0),
    JS_FN("divergeFromRecording", RecordReplay_DivergeFromRecording, 0, 0),
    JS_FN("progressCounter", RecordReplay_ProgressCounter, 0, 0),
    JS_FN("setProgressCounter", RecordReplay_SetProgressCounter, 1, 0),
    JS_FN("shouldUpdateProgressCounter",
          RecordReplay_ShouldUpdateProgressCounter, 1, 0),
    JS_FN("manifestFinished", RecordReplay_ManifestFinished, 1, 0),
    JS_FN("resumeExecution", RecordReplay_ResumeExecution, 0, 0),
    JS_FN("currentExecutionTime", RecordReplay_CurrentExecutionTime, 0, 0),
    JS_FN("flushRecording", RecordReplay_FlushRecording, 0, 0),
    JS_FN("flushExternalCalls", RecordReplay_FlushExternalCalls, 0, 0),
    JS_FN("setRecordingSummary", RecordReplay_SetRecordingSummary, 1, 0),
    JS_FN("getRecordingSummary", RecordReplay_GetRecordingSummary, 0, 0),
    JS_FN("getContent", RecordReplay_GetContent, 1, 0),
    JS_FN("getGraphics", RecordReplay_GetGraphics, 1, 0),
    JS_FN("isScanningScripts", RecordReplay_IsScanningScripts, 0, 0),
    JS_FN("setScanningScripts", RecordReplay_SetScanningScripts, 1, 0),
    JS_FN("getFrameDepth", RecordReplay_GetFrameDepth, 0, 0),
    JS_FN("setFrameDepth", RecordReplay_SetFrameDepth, 1, 0),
    JS_FN("onScriptHit", RecordReplay_OnScriptHit, 3, 0),
    JS_FN("onEnterFrame", RecordReplay_OnChangeFrame<ChangeFrameEnter>, 2, 0),
    JS_FN("onExitFrame", RecordReplay_OnChangeFrame<ChangeFrameExit>, 2, 0),
    JS_FN("onResumeFrame", RecordReplay_OnChangeFrame<ChangeFrameResume>, 2, 0),
    JS_FN("instrumentationCallback", RecordReplay_InstrumentationCallback, 3,
          0),
    JS_FN("findScriptHits", RecordReplay_FindScriptHits, 3, 0),
    JS_FN("findChangeFrames", RecordReplay_FindChangeFrames, 3, 0),
    JS_FN("getenv", RecordReplay_GetEnv, 1, 0),
    JS_FN("saveCloudRecording", RecordReplay_SaveCloudRecording, 1, 0),
    JS_FN("setUnhandledDivergenceAllowed", RecordReplay_SetUnhandledDivergenceAllowed, 1, 0),
    JS_FN("setCrashNote", RecordReplay_SetCrashNote, 1, 0),
    JS_FN("dump", RecordReplay_Dump, 1, 0),
    JS_FN("crash", RecordReplay_Crash, 0, 0),
    JS_FS_END};

extern "C" {

MOZ_EXPORT bool RecordReplayInterface_DefineRecordReplayControlObject(
    void* aCxVoid, void* aObjectArg) {
  JSContext* aCx = static_cast<JSContext*>(aCxVoid);
  RootedObject object(aCx, static_cast<JSObject*>(aObjectArg));

  RootedObject staticObject(aCx, JS_NewObject(aCx, nullptr));
  if (!staticObject ||
      !JS_DefineProperty(aCx, object, "RecordReplayControl", staticObject, 0)) {
    return false;
  }

  if (gModuleObject) {
    // RecordReplayControl objects created while setting up the module itself
    // don't get references to the module.
    RootedObject obj(aCx, *gModuleObject);
    if (!JS_WrapObject(aCx, &obj) ||
        !JS_DefineProperty(aCx, staticObject, "module", obj, 0)) {
      return false;
    }
  }

  if (IsMiddleman()) {
    if (!JS_DefineFunctions(aCx, staticObject, gMiddlemanMethods)) {
      return false;
    }
  } else {
    MOZ_RELEASE_ASSERT(IsRecordingOrReplaying());
    if (!JS_DefineFunctions(aCx, staticObject, gRecordReplayMethods)) {
      return false;
    }
  }

  return true;
}

}  // extern "C"

}  // namespace js
}  // namespace recordreplay
}  // namespace mozilla
