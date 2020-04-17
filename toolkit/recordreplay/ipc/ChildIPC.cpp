/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

// This file has the logic which the replayed process uses to communicate with
// the middleman process.

#include "ChildInternal.h"

#include "base/eintr_wrapper.h"
#include "base/message_loop.h"
#include "base/task.h"
#include "chrome/common/child_thread.h"
#include "chrome/common/mach_ipc_mac.h"
#include "ipc/Channel.h"
#include "mac/handler/exception_handler.h"
#include "mozilla/Base64.h"
#include "mozilla/Compression.h"
#include "mozilla/layers/ImageDataSerializer.h"
#include "mozilla/Sprintf.h"
#include "mozilla/VsyncDispatcher.h"

#include "InfallibleVector.h"
#include "nsPrintfCString.h"
#include "ParentInternal.h"
#include "ProcessRecordReplay.h"
#include "ProcessRedirect.h"
#include "ProcessRewind.h"
#include "Thread.h"
#include "Units.h"

#include "imgIEncoder.h"
#include "nsComponentManagerUtils.h"
#include "mozilla/BasicEvents.h"

#include <dlfcn.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <unordered_map>

namespace mozilla {
namespace recordreplay {
namespace child {

///////////////////////////////////////////////////////////////////////////////
// Record/Replay IPC
///////////////////////////////////////////////////////////////////////////////

// Monitor used for various synchronization tasks.
Monitor* gMonitor;

// The singleton channel for communicating with the middleman.
Channel* gChannel;

// IDs of the tree of processes this is part of.
static size_t gChildId;

// ID for this fork, or zero.
static size_t gForkId;

static base::ProcessId gMiddlemanPid;
static base::ProcessId gParentPid;
static StaticInfallibleVector<char*> gParentArgv;

// File descriptors used by a pipe to create checkpoints when instructed by the
// parent process.
static FileHandle gCheckpointWriteFd;
static FileHandle gCheckpointReadFd;

// Copy of the introduction message we got from the middleman. This is saved on
// receipt and then processed during InitRecordingOrReplayingProcess.
static UniquePtr<IntroductionMessage, Message::FreePolicy> gIntroductionMessage;

// Manifests which we've been sent but haven't processed yet. Protected by gMonitor.
static StaticInfallibleVector<js::CharBuffer*> gPendingManifests;

// Whether we are currently processing a manifest and can't start another one.
// Protected by gMonitor.
static bool gProcessingManifest = true;

// All recording contents we have received, protected by gMonitor. This may not
// have all been incorporated into the recording, which happens on the main
// thread.
static StaticInfallibleVector<char> gRecordingContents;

// Messages containing recording data which are not contiguous with the
// recording contents received so far.
static StaticInfallibleVector<Message::UniquePtr> gDeferredRecordingDataMessages;

// Any response received to the last ExternalCallRequest message.
static UniquePtr<ExternalCallResponseMessage, Message::FreePolicy>
    gCallResponseMessage;

// Whether some thread has sent an ExternalCallRequest and is waiting for
// gCallResponseMessage to be filled in.
static bool gWaitingForCallResponse;

static void MaybeStartNextManifest(const MonitorAutoLock& aProofOfLock);
static void SendMessageToForkedProcess(Message::UniquePtr aMsg, bool aLockHeld = false);
static void FetchCloudRecordingData(char** aBuffer, size_t* aSize);
static void HandleSharedKeyResponse(const SharedKeyResponseMessage& aMsg);
static void OnNewRecordingData(Message::UniquePtr aMsg);

// Lock which allows non-main threads to prevent forks. Readers are the threads
// preventing forks from happening, while the writer is the main thread during
// a fork.
static ReadWriteSpinLock gForkLock;

// Set when the process is shutting down, to suppress error reporting.
static AtomicBool gExitCalled;

// Processing routine for incoming channel messages.
static void ChannelMessageHandler(Message::UniquePtr aMsg) {
  if (aMsg->mForkId != gForkId) {
    AutoReadSpinLock disallowFork(gForkLock);
    if (gForkId) {
      // For some reason we can receive messages intended for another fork
      // which has terminated.
      Print("Warning: Ignoring message for fork %lu, current fork is %lu.\n",
            aMsg->mForkId, gForkId);
      return;
    }
    SendMessageToForkedProcess(std::move(aMsg));
    return;
  }

  // Handle critical messages without acquiring the fork lock. The main thread
  // could be stuck holding the fork lock and we need to immediately handle
  // requests to terminate.
  switch (aMsg->mType) {
    case MessageType::Terminate: {
      Print("Terminate message received, exiting...\n");
      gExitCalled = true;
      _exit(0);
      break;
    }
    case MessageType::Crash: {
      Print("Error: Crashing hanged process, dumping threads...\n");
      Thread::DumpThreads();
      ReportFatalError("Hung replaying process");
      break;
    }
    default:
      break;
  }

  AutoReadSpinLock disallowFork(gForkLock);

  switch (aMsg->mType) {
    case MessageType::Introduction: {
      MonitorAutoLock lock(*gMonitor);
      MOZ_RELEASE_ASSERT(!gIntroductionMessage);
      gIntroductionMessage.reset(
          static_cast<IntroductionMessage*>(aMsg.release()));
      gMonitor->NotifyAll();
      break;
    }
    case MessageType::CreateCheckpoint: {
      MOZ_RELEASE_ASSERT(IsRecording());

      // Ignore requests to create checkpoints before we have reached the first
      // paint and finished initializing.
      if (js::IsInitialized()) {
        uint8_t data = 0;
        DirectWrite(gCheckpointWriteFd, &data, 1);
      }
      break;
    }
    case MessageType::Ping: {
      // The progress value included in a ping response reflects both the JS
      // execution progress counter and the progress that all threads have
      // made in their event streams. This accounts for an assortment of
      // scenarios which could be mistaken for a hang, such as a long-running
      // script that doesn't interact with the recording, or a long-running
      // operation running off the main thread.
      const PingMessage& nmsg = (const PingMessage&)*aMsg;
      uint64_t total =
          *ExecutionProgressCounter() + Thread::TotalEventProgress();
      PrintLog("PingResponse %u %llu", nmsg.mId, total);
      gChannel->SendMessage(PingResponseMessage(gForkId, nmsg.mId, total));
      break;
    }
    case MessageType::ManifestStart: {
      PrintLog("ManifestQueued");
      MonitorAutoLock lock(*gMonitor);
      const ManifestStartMessage& nmsg = (const ManifestStartMessage&)*aMsg;
      NS_ConvertUTF8toUTF16 converted(nmsg.BinaryData(), nmsg.BinaryDataSize());
      js::CharBuffer* buf = new js::CharBuffer();
      buf->append(converted.get(), converted.Length());
      gPendingManifests.append(buf);
      MaybeStartNextManifest(lock);
      break;
    }
    case MessageType::ExternalCallResponse: {
      MonitorAutoLock lock(*gMonitor);
      MOZ_RELEASE_ASSERT(gWaitingForCallResponse);
      MOZ_RELEASE_ASSERT(!gCallResponseMessage);
      gCallResponseMessage.reset(
          static_cast<ExternalCallResponseMessage*>(aMsg.release()));
      gMonitor->NotifyAll();
      break;
    }
    case MessageType::ReplayJS: {
      MonitorAutoLock lock(*gMonitor);
      const ReplayJSMessage& nmsg = (const ReplayJSMessage&)*aMsg;

      // We are off the main thread, but this is OK because the recording data
      // itself has not been received yet, and the JS module will not be read
      // until we have reached the first checkpoint.
      MOZ_RELEASE_ASSERT(IsReplaying());
      js::SetWebReplayJS(nsCString(nmsg.BinaryData(), nmsg.BinaryDataSize()));
      break;
    }
    case MessageType::EnableLogging: {
      parent::gLoggingEnabled = true;
      break;
    }
    case MessageType::LogText: {
      const LogTextMessage& nmsg = (const LogTextMessage&)*aMsg;
      DirectPrint(nmsg.BinaryData());
      break;
    }
    case MessageType::RecordingData:
      OnNewRecordingData(std::move(aMsg));
      break;
    case MessageType::FetchCloudRecordingData: {
      MonitorAutoLock lock(*gMonitor);
      char* buf;
      size_t size;
      FetchCloudRecordingData(&buf, &size);
      gRecordingContents.append(buf, size);
      gMonitor->NotifyAll();
      break;
    }
    case MessageType::SharedKeyResponse: {
      const auto& nmsg = (const SharedKeyResponseMessage&)*aMsg;
      HandleSharedKeyResponse(nmsg);
      break;
    }
    default:
      MOZ_CRASH();
  }
}

// Main routine for a thread whose sole purpose is to listen to requests from
// the middleman process to create a new checkpoint. This is separate from the
// channel thread because this thread is recorded and the latter is not
// recorded. By communicating between the two threads with a pipe, this
// thread's behavior will be replicated exactly when replaying and new
// checkpoints will be created at the same point as during recording.
static void ListenForCheckpointThreadMain(void*) {
  while (true) {
    uint8_t data = 0;
    ssize_t rv = HANDLE_EINTR(read(gCheckpointReadFd, &data, 1));
    if (rv > 0) {
      NS_DispatchToMainThread(NewRunnableFunction("CreateCheckpoint", CreateCheckpoint));
    } else {
      MOZ_RELEASE_ASSERT(errno == EIO);
      MOZ_RELEASE_ASSERT(HasDivergedFromRecording());
      Thread::WaitForever();
    }
  }
}

// Shared memory block for graphics data.
void* gGraphicsShmem;

static void WaitForGraphicsShmem() {
  // Setup a mach port to receive the graphics shmem handle over.
  nsPrintfCString portString("WebReplay.%d.%lu", gMiddlemanPid, GetId());
  ReceivePort receivePort(portString.get());

  MachSendMessage handshakeMessage(parent::GraphicsHandshakeMessageId);
  handshakeMessage.AddDescriptor(
      MachMsgPortDescriptor(receivePort.GetPort(), MACH_MSG_TYPE_COPY_SEND));

  MachPortSender sender(nsPrintfCString("WebReplay.%d", gMiddlemanPid).get());
  kern_return_t kr = sender.SendMessage(handshakeMessage, 1000);
  MOZ_RELEASE_ASSERT(kr == KERN_SUCCESS);

  // The parent should send us a handle to the graphics shmem.
  MachReceiveMessage message;
  kr = receivePort.WaitForMessage(&message, 0);
  MOZ_RELEASE_ASSERT(kr == KERN_SUCCESS);
  MOZ_RELEASE_ASSERT(message.GetMessageID() == parent::GraphicsMemoryMessageId);
  mach_port_t graphicsPort = message.GetTranslatedPort(0);
  MOZ_RELEASE_ASSERT(graphicsPort != MACH_PORT_NULL);

  mach_vm_address_t address = 0;
  kr = mach_vm_map(mach_task_self(), &address, parent::GraphicsMemorySize, 0,
                   VM_FLAGS_ANYWHERE, graphicsPort, 0, false,
                   VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE,
                   VM_INHERIT_NONE);
  MOZ_RELEASE_ASSERT(kr == KERN_SUCCESS);

  gGraphicsShmem = (void*)address;
}

void SetupRecordReplayChannel(int aArgc, char* aArgv[]) {
  MOZ_RELEASE_ASSERT(IsRecordingOrReplaying() &&
                     AreThreadEventsPassedThrough());

  Maybe<int> channelID;
  for (int i = 0; i < aArgc; i++) {
    if (!strcmp(aArgv[i], gMiddlemanPidOption)) {
      MOZ_RELEASE_ASSERT(!gMiddlemanPid && i + 1 < aArgc);
      gMiddlemanPid = atoi(aArgv[i + 1]);
    }
    if (!strcmp(aArgv[i], gChannelIDOption)) {
      MOZ_RELEASE_ASSERT(channelID.isNothing() && i + 1 < aArgc);
      channelID.emplace(atoi(aArgv[i + 1]));
    }
  }
  MOZ_RELEASE_ASSERT(channelID.isSome());

  gMonitor = new Monitor();
  gChannel = new Channel(channelID.ref(), Channel::Kind::RecordReplay,
                         ChannelMessageHandler, gMiddlemanPid);
  gChildId = channelID.ref();

  // Wait for the parent to send us the introduction message.
  MonitorAutoLock lock(*gMonitor);
  while (!gIntroductionMessage) {
    gMonitor->Wait();
  }

  // If we're replaying, we also need to wait for some recording data.
  if (IsReplaying()) {
    while (gRecordingContents.empty()) {
      gMonitor->Wait();
    }
  }
}

static void InitializeForkListener();
static void InitializeSharedDatabase();

void InitRecordingOrReplayingProcess(int* aArgc, char*** aArgv) {
  if (!IsRecordingOrReplaying()) {
    return;
  }

  MOZ_RELEASE_ASSERT(!AreThreadEventsPassedThrough());

  {
    AutoPassThroughThreadEvents pt;
    if (IsRecording()) {
      WaitForGraphicsShmem();
    } else {
      InitializeForkListener();
      InitializeSharedDatabase();
    }
  }

  DirectCreatePipe(&gCheckpointWriteFd, &gCheckpointReadFd);
  Thread::StartThread(ListenForCheckpointThreadMain, nullptr, false);

  // Process the introduction message to fill in arguments.
  MOZ_RELEASE_ASSERT(gParentArgv.empty());

  // Record/replay the introduction message itself so we get consistent args
  // between recording and replaying.
  {
    IntroductionMessage* msg =
        IntroductionMessage::RecordReplay(*gIntroductionMessage);

    gParentPid = gIntroductionMessage->mParentPid;

    const char* pos = msg->ArgvString();
    for (size_t i = 0; i < msg->mArgc; i++) {
      gParentArgv.append(strdup(pos));
      pos += strlen(pos) + 1;
    }

    free(msg);
  }

  gIntroductionMessage = nullptr;

  // Some argument manipulation code expects a null pointer at the end.
  gParentArgv.append(nullptr);

  MOZ_RELEASE_ASSERT(*aArgc >= 1);
  MOZ_RELEASE_ASSERT(gParentArgv.back() == nullptr);

  *aArgc = gParentArgv.length() - 1;  // For the trailing null.
  *aArgv = gParentArgv.begin();
}

base::ProcessId MiddlemanProcessId() { return gMiddlemanPid; }

base::ProcessId ParentProcessId() { return gParentPid; }

static void HandleMessageFromForkedProcess(Message::UniquePtr aMsg);

// Messages to send to forks that don't exist yet. Protected by gMonitor.
static StaticInfallibleVector<Message::UniquePtr> gPendingForkMessages;

struct ForkedProcess {
  base::ProcessId mPid;
  size_t mForkId;
  Channel* mChannel;
};

// Indexed by fork ID. Protected by gMonitor.
static StaticInfallibleVector<ForkedProcess*> gForkedProcesses;

static FileHandle gForkWriteFd, gForkReadFd;
static char* gFatalErrorMemory;
static const size_t FatalErrorMemorySize = PageSize * 4;

static void ForkListenerThread(void*) {
  while (true) {
    ForkedProcess process;
    int nbytes = read(gForkReadFd, &process, sizeof(process));
    MOZ_RELEASE_ASSERT(nbytes == sizeof(process));

    PrintLog("ConnectedToFork %lu", process.mForkId);

    AutoReadSpinLock disallowFork(gForkLock);
    MonitorAutoLock lock(*gMonitor);

    process.mChannel = new Channel(0, Channel::Kind::ReplayRoot,
                                   HandleMessageFromForkedProcess,
                                   process.mPid);

    // Send any messages destined for this fork.
    size_t i = 0;
    while (i < gPendingForkMessages.length()) {
      auto& pending = gPendingForkMessages[i];
      if (pending->mForkId == process.mForkId) {
        process.mChannel->SendMessage(std::move(*pending));
        gPendingForkMessages.erase(&pending);
      } else {
        i++;
      }
    }

    while (process.mForkId >= gForkedProcesses.length()) {
      gForkedProcesses.append(nullptr);
    }
    MOZ_RELEASE_ASSERT(!gForkedProcesses[process.mForkId]);
    gForkedProcesses[process.mForkId] = new ForkedProcess(process);
  }
}

static void InitializeForkListener() {
  DirectCreatePipe(&gForkWriteFd, &gForkReadFd);

  Thread::SpawnNonRecordedThread(ForkListenerThread, nullptr);

  if (!ReplayingInCloud()) {
    gFatalErrorMemory = (char*) mmap(nullptr, FatalErrorMemorySize,
                                     PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
    MOZ_RELEASE_ASSERT(gFatalErrorMemory != MAP_FAILED);
  }
}

static void SendMessageToForkedProcess(Message::UniquePtr aMsg, bool aLockHeld) {
  if (IsVerbose() && aMsg->mType == MessageType::ManifestStart) {
    PrintLog("SendManifestStartToForkedProcess %u %u", aMsg->mSize, aMsg->Hash());
  }

  Maybe<MonitorAutoLock> lock;
  if (!aLockHeld) {
    lock.emplace(*gMonitor);
  }

  if (aMsg->mForkId < gForkedProcesses.length() && gForkedProcesses[aMsg->mForkId]) {
    ForkedProcess* process = gForkedProcesses[aMsg->mForkId];
    bool remove =
        aMsg->mType == MessageType::Terminate ||
        aMsg->mType == MessageType::Crash;
    process->mChannel->SendMessage(std::move(*aMsg));
    if (remove) {
      delete process;
      gForkedProcesses[aMsg->mForkId] = nullptr;
    }
    return;
  }

  gPendingForkMessages.append(std::move(aMsg));
}

static void HandleSharedKeySet(const SharedKeySetMessage& aMsg);
static void HandleSharedKeyRequest(const SharedKeyRequestMessage& aMsg);

static void HandleMessageFromForkedProcess(Message::UniquePtr aMsg) {
  // Certain messages from forked processes are intended for this one,
  // instead of the middleman.
  AutoReadSpinLock disallowFork(gForkLock);

  switch (aMsg->mType) {
    case MessageType::UpdateRecordingFromRoot: {
      const auto& nmsg = static_cast<const UpdateRecordingFromRootMessage&>(*aMsg);
      EnsureRecordingLength(nmsg.mRequiredLength);

      MonitorAutoLock lock(*gMonitor);
      Message::UniquePtr newMessage(RecordingDataMessage::New(
          nmsg.mForkId, nmsg.mStart,
          gRecordingContents.begin() + nmsg.mStart,
          nmsg.mRequiredLength - nmsg.mStart));
      SendMessageToForkedProcess(std::move(newMessage), /* aLockHeld */ true);
      break;
    }
    case MessageType::ExternalCallRequest: {
      const auto& nmsg = static_cast<const ExternalCallRequestMessage&>(*aMsg);

      InfallibleVector<char> outputData;
      if (HasExternalCallOutput(nmsg.mTag, &outputData)) {
        Message::UniquePtr response(ExternalCallResponseMessage::New(
            nmsg.mForkId, nmsg.mTag, outputData.begin(), outputData.length()));
        SendMessageToForkedProcess(std::move(response));
        return;
      }

      // The call result was not found.
      Message::UniquePtr response(ExternalCallResponseMessage::New(
          nmsg.mForkId, 0, nullptr, 0));
      SendMessageToForkedProcess(std::move(response));
      return;
    }
    case MessageType::ExternalCallResponse: {
      const auto& nmsg = static_cast<const ExternalCallResponseMessage&>(*aMsg);
      AddExternalCallOutput(nmsg.mTag, nmsg.BinaryData(), nmsg.BinaryDataSize());
      return;
    }
    case MessageType::ScanData:
      js::AddScanDataMessage(std::move(aMsg));
      return;
    case MessageType::SharedKeySet: {
      const auto& nmsg = static_cast<const SharedKeySetMessage&>(*aMsg);
      HandleSharedKeySet(nmsg);
      return;
    }
    case MessageType::SharedKeyRequest: {
      const auto& nmsg = static_cast<const SharedKeyRequestMessage&>(*aMsg);
      HandleSharedKeyRequest(nmsg);
      return;
    }
    default:
      break;
  }

  gChannel->SendMessage(std::move(*aMsg));
}

static const size_t ForkTimeoutSeconds = 10;

void PerformFork(size_t aForkId) {
  PrintLog("PerformFork Start");
  gForkLock.WriteLock();
  PrintLog("PerformFork Locked");

  if (ForkProcess(aForkId)) {
    // This is the original process.
    gForkLock.WriteUnlock();
    return;
  }

  // We need to reset the fork lock, but its internal spin lock might be held by
  // a thread which no longer exists. Reset the lock instead of unlocking it
  // to avoid deadlocking in this case.
  PodZero(&gForkLock);

  AutoPassThroughThreadEvents pt;

  // Any pending manifests we have are for the original process. We can start
  // getting new manifests for this process once we've registered our channel,
  // so clear out the obsolete pending manifests first.
  {
    MonitorAutoLock lock(*gMonitor);
    gPendingManifests.clear();
  }

  gForkId = aForkId;
  gChannel = new Channel(0, Channel::Kind::ReplayForked, ChannelMessageHandler);

  ForkedProcess process;
  process.mPid = getpid();
  process.mForkId = aForkId;
  int nbytes = write(gForkWriteFd, &process, sizeof(process));
  MOZ_RELEASE_ASSERT(nbytes == sizeof(process));

  // If the root process is exiting while we are setting up the channel, it will
  // not connect to this process and we won't be able to shut down properly.
  // Set a timeout to avoid this situation.
  TimeStamp deadline =
      TimeStamp::Now() + TimeDuration::FromSeconds(ForkTimeoutSeconds);
  gChannel->ExitIfNotInitializedBefore(deadline);
}

template <MessageType Type>
static ErrorMessage<Type>* ConstructErrorMessageOnStack(
    char* aBuf, size_t aSize, size_t aForkId, const char* aMessage) {
  size_t header = sizeof(ErrorMessage<Type>);
  size_t len = std::min(strlen(aMessage) + 1, aSize - header);
  ErrorMessage<Type>* msg = new (aBuf) ErrorMessage<Type>(header + len, aForkId);
  memcpy(&aBuf[header], aMessage, len);
  aBuf[aSize - 1] = 0;
  return msg;
}

static void SendFatalErrorMessage(size_t aForkId, const char* aMessage) {
  // Construct a FatalErrorMessage on the stack, to avoid touching the heap.
  char msgBuf[4096];
  FatalErrorMessage* msg =
      ConstructErrorMessageOnStack<MessageType::FatalError>(
          msgBuf, sizeof(msgBuf), aForkId, aMessage);

  gChannel->SendMessage(std::move(*msg));

  Print("***** Fatal Record/Replay Error #%lu:%lu *****\n%s\n", GetId(), aForkId,
        aMessage);
}

void ReportCrash(const MinidumpInfo& aInfo, void* aFaultingAddress) {
  int pid;
  pid_for_task(aInfo.mTask, &pid);

  uint32_t forkId = UINT32_MAX;
  if (aInfo.mTask != mach_task_self()) {
    for (const ForkedProcess* fork : gForkedProcesses) {
      if (fork && fork->mPid == pid) {
        forkId = fork->mForkId;
      }
    }
    if (forkId == UINT32_MAX) {
      Print("Could not find fork ID for crashing task\n");
    }
  }

  AutoEnsurePassThroughThreadEvents pt;

#ifdef MOZ_CRASHREPORTER
  google_breakpad::ExceptionHandler::WriteForwardedExceptionMinidump(
      aInfo.mExceptionType, aInfo.mCode, aInfo.mSubcode, aInfo.mThread,
      aInfo.mTask);
#endif

  char buf[2048];
  if (gFatalErrorMemory && gFatalErrorMemory[0]) {
    SprintfLiteral(buf, "%s", gFatalErrorMemory);
    memset(gFatalErrorMemory, 0, FatalErrorMemorySize);
  } else {
    SprintfLiteral(buf, "Fault %p", aFaultingAddress);
  }

  SendFatalErrorMessage(forkId, buf);
}

void ReportFatalError(const char* aFormat, ...) {
  if (gExitCalled) {
    return;
  }

  if (!gFatalErrorMemory) {
    gFatalErrorMemory = new char[FatalErrorMemorySize];
  }

  va_list ap;
  va_start(ap, aFormat);
  vsnprintf(gFatalErrorMemory, FatalErrorMemorySize - 1, aFormat, ap);
  va_end(ap);

  Print("BeginFatalError\n");
  DirectPrint(gFatalErrorMemory);
  DirectPrint("\nEndFatalError\n");

  MOZ_CRASH("ReportFatalError");
}

extern "C" {

// When running in the cloud the translation layer detects crashes that have
// occurred in the current process, and uses this interface to report those
// crashes to the middleman.
MOZ_EXPORT void RecordReplayInterface_ReportCrash(const char* aMessage) {
  SendFatalErrorMessage(gForkId, aMessage);
}

} // extern "C"

void ReportCriticalError(const char* aMessage) {
  char msgBuf[4096];
  CriticalErrorMessage* msg =
      ConstructErrorMessageOnStack<MessageType::CriticalError>(
          msgBuf, sizeof(msgBuf), gForkId, aMessage);
  gChannel->SendMessage(std::move(*msg));

  Print("Critical Error: %s\n", aMessage);
}

static bool gUnhandledDivergenceAllowed = true;

void SetUnhandledDivergenceAllowed(bool aAllowed) {
  gUnhandledDivergenceAllowed = aAllowed;
}

void ReportUnhandledDivergence() {
  if (!Thread::CurrentIsMainThread() || !gUnhandledDivergenceAllowed) {
    ReportFatalError("Unhandled divergence not allowed");
  }

  gChannel->SendMessage(UnhandledDivergenceMessage(gForkId));

  // Block until we get a terminate message and die.
  Thread::WaitForeverNoIdle();
}

size_t GetId() { return gChildId; }
size_t GetForkId() { return gForkId; }

static bool IncorporateRecordingData(const RecordingDataMessage& aMsg) {
  if (aMsg.mTag > gRecordingContents.length()) {
    return false;
  }

  size_t extent = aMsg.mTag + aMsg.BinaryDataSize();
  if (extent > gRecordingContents.length()) {
    size_t nbytes = extent - gRecordingContents.length();
    gRecordingContents.append(aMsg.BinaryData() + aMsg.BinaryDataSize() - nbytes,
                              nbytes);
  }

  return true;
}

void OnNewRecordingData(Message::UniquePtr aMsg) {
  MonitorAutoLock lock(*gMonitor);

  const auto& nmsg = (const RecordingDataMessage&)*aMsg;
  PrintLog("NewRecordingData %llu %lu", nmsg.mTag, nmsg.BinaryDataSize());

  if (IncorporateRecordingData(nmsg)) {
    for (size_t i = 0; i < gDeferredRecordingDataMessages.length();) {
      auto& deferred = gDeferredRecordingDataMessages[i];
      const auto& ndeferred = (const RecordingDataMessage&)*deferred;
      if (IncorporateRecordingData(ndeferred)) {
        PrintLog("AddDeferredRecordingData %llu", gRecordingContents.length());
        gDeferredRecordingDataMessages.erase(&deferred);
      } else {
        i++;
      }
    }
    PrintLog("NewRecordingData NotifyMonitor %p", gMonitor);
    gMonitor->NotifyAll();
  } else {
    // Defer processing this until it is contiguous with the earlier contents.
    PrintLog("DeferRecordingData");
    gDeferredRecordingDataMessages.append(std::move(aMsg));
  }
}

void AddPendingRecordingData(bool aRequireMore) {
  MOZ_RELEASE_ASSERT(Thread::CurrentIsMainThread());
  if (!NeedRespawnThreads()) {
    Thread::WaitForIdleThreads();
  }

  InfallibleVector<Stream*> updatedStreams;
  {
    MonitorAutoLock lock(*gMonitor);

    if (gRecordingContents.length() == gRecording->Size()) {
      if (aRequireMore) {
        Print("Hit end of recording (%lu bytes, checkpoint %lu, position %lu), crashing...\n",
              gRecordingContents.length(), GetLastCheckpoint(),
              Thread::Current()->Events().StreamPosition());

        nsAutoCString chunks;
        Thread::Current()->Events().PrintChunks(chunks);
        Print("Chunks %s\n", chunks.get());

        MOZ_CRASH("AddPendingRecordingData");
      }
    } else {
      gRecording->NewContents(
          (const uint8_t*)gRecordingContents.begin() + gRecording->Size(),
          gRecordingContents.length() - gRecording->Size(),
          &updatedStreams);
    }
  }

  for (Stream* stream : updatedStreams) {
    if (stream->Name() == StreamName::Lock) {
      Lock::LockAcquiresUpdated(stream->NameIndex());
    }
  }

  if (!NeedRespawnThreads()) {
    Thread::ResumeIdleThreads();
  }
}

void SaveCloudRecording(const char* aName) {
  MonitorAutoLock lock(*gMonitor);
  AutoEnsurePassThroughThreadEvents pt;

  static void* ptr = dlsym(RTLD_DEFAULT, "RecordReplay_SaveCloudRecording");
  if (ptr) {
    BitwiseCast<void(*)(const char*, const char*, size_t)>(ptr)(
        aName, gRecordingContents.begin(), gRecordingContents.length());
  } else {
    // Fallback for offline testing.
    const char* offlineDir = getenv("WEBREPLAY_OFFLINE");
    if (!offlineDir) {
      Print("WEBREPLAY_OFFLINE not set, crashing...\n");
      MOZ_CRASH("SaveCloudRecording");
    }
    nsPrintfCString path("%s/%s", offlineDir, aName);

    FileHandle file = DirectOpenFile(path.get(), /* aWriting */ true);
    DirectWrite(file, gRecordingContents.begin(), gRecordingContents.length());
    DirectCloseFile(file);
  }
}

static void FetchCloudRecordingData(char** aBuffer, size_t* aSize) {
  static void* ptr = dlsym(RTLD_DEFAULT, "RecordReplay_LoadCloudRecording");
  if (ptr) {
    BitwiseCast<void(*)(char**, size_t*)>(ptr)(aBuffer, aSize);
  } else {
    // Fallback for offline testing.
    nsAutoCString recordingName;
    ExtractCloudRecordingName(gRecordingFilename, recordingName);
    MOZ_RELEASE_ASSERT(!recordingName.IsEmpty());

    const char* offlineDir = getenv("WEBREPLAY_OFFLINE");
    if (!offlineDir) {
      Print("WEBREPLAY_OFFLINE not set, crashing...\n");
      MOZ_CRASH("SaveCloudRecording");
    }
    nsPrintfCString path("%s/%s", offlineDir, recordingName.get());

    FileHandle file = DirectOpenFile(path.get(), /* aWriting */ false);
    *aSize = DirectFileSize(file);
    *aBuffer = (char*) malloc(*aSize);
    DirectRead(file, *aBuffer, *aSize);
    DirectCloseFile(file);
  }
}

void SetCrashNote(const char* aNote) {
  MOZ_RELEASE_ASSERT(Thread::CurrentIsMainThread());
  void* ptr = dlsym(RTLD_DEFAULT, "RecordReplay_SetCrashNote");
  if (ptr) {
    BitwiseCast<void(*)(const char*)>(ptr)(aNote);
  }
}

uint64_t GetMemoryUsage() {
  MOZ_RELEASE_ASSERT(Thread::CurrentIsMainThread());
  void* ptr = dlsym(RTLD_DEFAULT, "RecordReplay_GetMemoryUsage");
  if (ptr) {
    return BitwiseCast<uint64_t(*)()>(ptr)();
  }
  return 0;
}

// In the middleman, JS to send to new replaying processes. This matches up
// with the control JS running in this process.
nsCString gReplayJS;

void SetWebReplayJS(const nsCString& aControlJS, const nsCString& aReplayJS) {
  if (IsMiddleman()) {
    js::SetWebReplayJS(aControlJS);
    gReplayJS = aReplayJS;
  } else if (IsRecording()) {
    js::SetWebReplayJS(aReplayJS);
  }
}

void PrintLog(const nsAString& aText) {
  double elapsed = ElapsedTime();
  NS_ConvertUTF16toUTF8 ntext(aText);
  if (IsRecording()) {
    nsPrintfCString buf("[Recording %.3f] %s\n", elapsed, ntext.get());
    UniquePtr<Message> msg(LogTextMessage::New(
        0, 0, buf.BeginReading(), buf.Length() + 1));
    gChannel->SendMessage(std::move(*msg));
  } else {
    nsPrintfCString buf("[#%lu %.3f] %s\n", gForkId, elapsed, ntext.get());
    DirectPrint(buf.get());
  }
}

void PrintLog(const char* aFormat, ...) {
  nsString str;
  va_list ap;
  va_start(ap, aFormat);
  str.AppendPrintf(aFormat, ap);
  va_end(ap);
  PrintLog(str);
}

///////////////////////////////////////////////////////////////////////////////
// Shared key-value database
///////////////////////////////////////////////////////////////////////////////

static Monitor* gSharedDatabaseMonitor;

// Used in root replaying process, protected by gSharedDatabaseMonitor.
typedef std::unordered_map<std::string, std::string> SharedDatabase;
static SharedDatabase* gSharedDatabase;

// Used in forked replaying processes, protected by gSharedDatabaseMonitor.
static Maybe<nsAutoCString> gSharedKeyResponse;

static void InitializeSharedDatabase() {
  gSharedDatabase = new SharedDatabase();
  gSharedDatabaseMonitor = new Monitor();
}

static void HandleSharedKeySet(const SharedKeySetMessage& aMsg) {
  MOZ_RELEASE_ASSERT(gForkId == 0);

  MonitorAutoLock lock(*gSharedDatabaseMonitor);

  std::string key(aMsg.BinaryData(), aMsg.mTag);
  std::string value(aMsg.BinaryData() + aMsg.mTag, aMsg.BinaryDataSize() - aMsg.mTag);
  gSharedDatabase->erase(key);
  gSharedDatabase->insert({ key, value });
}

static void HandleSharedKeyRequest(const SharedKeyRequestMessage& aMsg) {
  MOZ_RELEASE_ASSERT(gForkId == 0);

  Maybe<MonitorAutoLock> lock;
  lock.emplace(*gSharedDatabaseMonitor);

  std::string key(aMsg.BinaryData(), aMsg.BinaryDataSize());
  std::string value;

  const auto& entry = gSharedDatabase->find(key);
  if (entry != gSharedDatabase->end()) {
    value = entry->second;
  }

  Message::UniquePtr response(SharedKeyResponseMessage::New(
      aMsg.mForkId, 0, value.data(), value.length()));
  lock.reset();

  SendMessageToForkedProcess(std::move(response));
}

void SetSharedKey(const nsAutoCString& aKey, const nsAutoCString& aValue) {
  MOZ_RELEASE_ASSERT(gForkId != 0);
  MOZ_RELEASE_ASSERT(NS_IsMainThread());

  nsAutoCString combined;
  combined.Append(aKey);
  combined.Append(aValue);
  UniquePtr<Message> msg(SharedKeySetMessage::New(
      gForkId, aKey.Length(), combined.BeginReading(), combined.Length()));
  gChannel->SendMessage(std::move(*msg));
}

static void HandleSharedKeyResponse(const SharedKeyResponseMessage& aMsg) {
  MOZ_RELEASE_ASSERT(gForkId != 0);
  MOZ_RELEASE_ASSERT(!NS_IsMainThread());

  MonitorAutoLock lock(*gSharedDatabaseMonitor);

  MOZ_RELEASE_ASSERT(gSharedKeyResponse.isNothing());
  gSharedKeyResponse.emplace(aMsg.BinaryData(), aMsg.BinaryDataSize());
  gSharedDatabaseMonitor->Notify();
}

void GetSharedKey(const nsAutoCString& aKey, nsAutoCString& aValue) {
  MOZ_RELEASE_ASSERT(gForkId != 0);
  MOZ_RELEASE_ASSERT(NS_IsMainThread());

  UniquePtr<Message> msg(SharedKeyRequestMessage::New(
      gForkId, 0, aKey.BeginReading(), aKey.Length()));
  gChannel->SendMessage(std::move(*msg));

  MonitorAutoLock lock(*gSharedDatabaseMonitor);
  while (gSharedKeyResponse.isNothing()) {
    gSharedDatabaseMonitor->Wait();
  }
  aValue = *gSharedKeyResponse;
  gSharedKeyResponse.reset();
}

///////////////////////////////////////////////////////////////////////////////
// Vsyncs
///////////////////////////////////////////////////////////////////////////////

static VsyncObserver* gVsyncObserver;

void SetVsyncObserver(VsyncObserver* aObserver) {
  MOZ_RELEASE_ASSERT(!gVsyncObserver || !aObserver);
  gVsyncObserver = aObserver;
}

void NotifyVsyncObserver() {
  if (gVsyncObserver) {
    static VsyncId vsyncId;
    vsyncId = vsyncId.Next();
    VsyncEvent event(vsyncId, TimeStamp::Now());
    gVsyncObserver->NotifyVsync(event);
  }
}

// How many paints have been started and haven't reached PaintFromMainThread
// yet. Only accessed on the main thread.
static int32_t gNumPendingMainThreadPaints;

bool OnVsync() {
  // In the repainting stress mode, we create a new checkpoint on every vsync
  // message received from the UI process. When we notify the parent about the
  // new checkpoint it will trigger a repaint to make sure that all layout and
  // painting activity can occur when diverged from the recording.
  if (parent::InRepaintStressMode()) {
    CreateCheckpoint();
  }

  // After a paint starts, ignore incoming vsyncs until the paint completes.
  return gNumPendingMainThreadPaints == 0;
}

///////////////////////////////////////////////////////////////////////////////
// Painting
///////////////////////////////////////////////////////////////////////////////

// Target buffer for the draw target created by the child process widget, which
// the compositor thread writes to.
static void* gDrawTargetBuffer;
static size_t gDrawTargetBufferSize;

// Dimensions of the last paint which the compositor performed.
static size_t gPaintWidth, gPaintHeight;

// How many updates have been sent to the compositor thread and haven't been
// processed yet. This can briefly become negative if the main thread sends an
// update and the compositor processes it before the main thread reaches
// NotifyPaintStart. Outside of this window, the compositor can only write to
// gDrawTargetBuffer or update gPaintWidth/gPaintHeight if this is non-zero.
static Atomic<int32_t, SequentiallyConsistent, Behavior::DontPreserve>
    gNumPendingPaints;

// ID of the compositor thread.
static Atomic<size_t, SequentiallyConsistent, Behavior::DontPreserve>
    gCompositorThreadId;

already_AddRefed<gfx::DrawTarget> DrawTargetForRemoteDrawing(
    LayoutDeviceIntSize aSize) {
  MOZ_RELEASE_ASSERT(!NS_IsMainThread());

  // Keep track of the compositor thread ID.
  size_t threadId = Thread::Current()->Id();
  if (gCompositorThreadId) {
    MOZ_RELEASE_ASSERT(threadId == gCompositorThreadId);
  } else {
    gCompositorThreadId = threadId;
  }

  if (aSize.IsEmpty()) {
    return nullptr;
  }

  gPaintWidth = aSize.width;
  gPaintHeight = aSize.height;

  gfx::IntSize size(aSize.width, aSize.height);
  size_t bufferSize =
      layers::ImageDataSerializer::ComputeRGBBufferSize(size, gSurfaceFormat);
  MOZ_RELEASE_ASSERT(bufferSize <= parent::GraphicsMemorySize);

  if (bufferSize != gDrawTargetBufferSize) {
    free(gDrawTargetBuffer);
    gDrawTargetBuffer = malloc(bufferSize);
    gDrawTargetBufferSize = bufferSize;
  }

  size_t stride = layers::ImageDataSerializer::ComputeRGBStride(gSurfaceFormat,
                                                                aSize.width);
  RefPtr<gfx::DrawTarget> drawTarget = gfx::Factory::CreateDrawTargetForData(
      gfx::BackendType::SKIA, (uint8_t*)gDrawTargetBuffer, size, stride,
      gSurfaceFormat,
      /* aUninitialized = */ true);
  if (!drawTarget) {
    MOZ_CRASH();
  }

  return drawTarget.forget();
}

static bool EncodeGraphics(const nsACString& aMimeType,
                           const nsACString& aEncodeOptions,
                           nsACString& aData) {
  AutoPassThroughThreadEvents pt;

  // Get an image encoder for the media type.
  nsPrintfCString encoderCID("@mozilla.org/image/encoder;2?type=%s",
                             nsCString(aMimeType).get());
  nsCOMPtr<imgIEncoder> encoder = do_CreateInstance(encoderCID.get());

  size_t stride = layers::ImageDataSerializer::ComputeRGBStride(gSurfaceFormat,
                                                                gPaintWidth);

  nsString options = NS_ConvertUTF8toUTF16(aEncodeOptions);
  nsresult rv = encoder->InitFromData(
      (const uint8_t*)gDrawTargetBuffer, stride * gPaintHeight, gPaintWidth,
      gPaintHeight, stride, imgIEncoder::INPUT_FORMAT_HOSTARGB, options);
  if (NS_FAILED(rv)) {
    return false;
  }

  uint64_t count;
  rv = encoder->Available(&count);
  if (NS_FAILED(rv)) {
    return false;
  }

  rv = Base64EncodeInputStream(encoder, aData, count);
  return NS_SUCCEEDED(rv);
}

void NotifyPaintStart() {
  MOZ_RELEASE_ASSERT(NS_IsMainThread());

  // Initialize state on the first paint.
  static bool gPainted;
  if (!gPainted) {
    gPainted = true;
  }

  gNumPendingPaints++;
  gNumPendingMainThreadPaints++;
}

static void PaintFromMainThread() {
  MOZ_RELEASE_ASSERT(NS_IsMainThread());

  gNumPendingMainThreadPaints--;

  if (gNumPendingMainThreadPaints) {
    // Another paint started before we were able to finish it here. The draw
    // target buffer no longer reflects program state at the last checkpoint,
    // so don't send a Paint message.
    return;
  }

  // If all paints have completed, the compositor cannot be simultaneously
  // operating on the draw target buffer.
  MOZ_RELEASE_ASSERT(!gNumPendingPaints);

  if (IsRecording() && gDrawTargetBuffer) {
    memcpy(gGraphicsShmem, gDrawTargetBuffer, gDrawTargetBufferSize);
    gChannel->SendMessage(PaintMessage(gPaintWidth, gPaintHeight));
  }

  if (IsReplaying() && !HasDivergedFromRecording()) {
    js::PaintComplete();
  }
}

void NotifyPaintComplete() {
  MOZ_RELEASE_ASSERT(!gCompositorThreadId ||
                     Thread::Current()->Id() == gCompositorThreadId);

  // Notify the main thread in case it is waiting for this paint to complete.
  {
    MonitorAutoLock lock(*gMonitor);
    if (--gNumPendingPaints == 0) {
      gMonitor->Notify();
    }
  }

  // Notify the middleman about the completed paint from the main thread.
  NS_DispatchToMainThread(
      NewRunnableFunction("PaintFromMainThread", PaintFromMainThread));
}

// Whether we have repainted since diverging from the recording.
static bool gDidRepaint;

bool GetGraphics(bool aRepaint, const nsACString& aMimeType,
                 const nsACString& aEncodeOptions, nsACString& aData) {
  MOZ_RELEASE_ASSERT(NS_IsMainThread());

  EnsureNonMainThreadsAreSpawned();

  // Don't try to repaint if the first normal paint hasn't occurred yet.
  if (!gCompositorThreadId) {
    return false;
  }

  if (aRepaint) {
    MOZ_RELEASE_ASSERT(IsReplaying());
    MOZ_RELEASE_ASSERT(HasDivergedFromRecording());

    // Ignore the request to repaint if we already triggered a repaint, in which
    // case the last graphics we sent will still be correct.
    if (!gDidRepaint) {
      gDidRepaint = true;

      // Create an artifical vsync to see if graphics have changed since the
      // last paint and a new paint is needed.
      NotifyVsyncObserver();

      // Wait for the compositor to finish all in flight paints, including any
      // one we just triggered.
      {
        MonitorAutoLock lock(*gMonitor);
        while (gNumPendingPaints) {
          gMonitor->Wait();
        }
      }
    }
  } else {
    // We don't have a good way of making sure this assert passes when saving
    // recording summaries.
    MOZ_RELEASE_ASSERT(!gNumPendingMainThreadPaints);
  }

  if (!gDrawTargetBuffer) {
    return false;
  }

  return EncodeGraphics(aMimeType, aEncodeOptions, aData);
}

bool PaintingInProgress() {
  MOZ_RELEASE_ASSERT(NS_IsMainThread());
  return gNumPendingMainThreadPaints != 0;
}

///////////////////////////////////////////////////////////////////////////////
// Message Helpers
///////////////////////////////////////////////////////////////////////////////

void EnsureRecordingLength(size_t aLength) {
  MonitorAutoLock lock(*gMonitor);

  if (gForkId && gRecordingContents.length() < aLength) {
    gChannel->SendMessage(UpdateRecordingFromRootMessage(
        gForkId, gRecordingContents.length(), aLength));
  }

  while (gRecordingContents.length() < aLength) {
    PrintLog("EnsureRecordingLength %p have %lu need %lu, waiting...",
             gMonitor, gRecordingContents.length(), aLength);
    gMonitor->Wait();
  }
  PrintLog("EnsureRecordingLength done %lu", gRecordingContents.length());
}

static void MaybeStartNextManifest(const MonitorAutoLock& aProofOfLock) {
  if (!gPendingManifests.empty() && !gProcessingManifest) {
    js::CharBuffer* buf = gPendingManifests[0];
    gPendingManifests.erase(&gPendingManifests[0]);
    gProcessingManifest = true;
    PauseMainThreadAndInvokeCallback([=]() {
      js::ManifestStart(*buf);
      delete buf;
    });
  }
}

#undef compress

void ManifestFinished(const js::CharBuffer& aBuffer, bool aBulk, bool aCompress) {
  MOZ_RELEASE_ASSERT(NS_IsMainThread());
  MOZ_RELEASE_ASSERT(gProcessingManifest);

  NS_ConvertUTF16toUTF8 converted(aBuffer.begin(), aBuffer.length());

  ManifestFinishedMessage* msg;
  if (aCompress) {
    char* compressed = new char[Compression::LZ4::maxCompressedSize(converted.Length())];
    unsigned length = Compression::LZ4::compress(converted.get(), converted.Length(), compressed);
    msg = ManifestFinishedMessage::New(gForkId, converted.Length(), compressed, length);
    delete[] compressed;

    PrintLog("CompressedMessage %u %u", converted.Length(), length);
  } else {
    msg = ManifestFinishedMessage::New(gForkId, 0, converted.get(), converted.Length());
  }
  if (aBulk) {
    msg->SetBulk();
  }

  if (IsVerbose()) {
    nsPrintfCString logMessage("ManifestFinishedHash %lu %u %u\n",
                               GetForkId(), msg->mSize, msg->Hash());
    Print(logMessage.get());
  }

  PauseMainThreadAndInvokeCallback([=]() {
    gChannel->SendMessage(std::move(*msg));
    free(msg);

    MonitorAutoLock lock(*gMonitor);
    gProcessingManifest = false;
    MaybeStartNextManifest(lock);
  });
}

void SendExternalCallRequest(ExternalCallId aId,
                             const char* aInputData, size_t aInputSize,
                             InfallibleVector<char>* aOutputData,
                             bool* aOutputUnavailable) {
  AutoPassThroughThreadEvents pt;
  MonitorAutoLock lock(*gMonitor);

  while (gWaitingForCallResponse) {
    gMonitor->Wait();
  }
  gWaitingForCallResponse = true;

  UniquePtr<ExternalCallRequestMessage> msg(ExternalCallRequestMessage::New(
      gForkId, aId, aInputData, aInputSize));
  gChannel->SendMessage(std::move(*msg));

  while (!gCallResponseMessage) {
    gMonitor->Wait();
  }

  aOutputData->append(gCallResponseMessage->BinaryData(),
                      gCallResponseMessage->BinaryDataSize());
  if (!gCallResponseMessage->mTag) {
    *aOutputUnavailable = true;
  }

  gCallResponseMessage = nullptr;
  gWaitingForCallResponse = false;

  gMonitor->Notify();
}

void SendExternalCallOutput(ExternalCallId aId,
                            const char* aOutputData, size_t aOutputSize) {
  Message::UniquePtr msg(ExternalCallResponseMessage::New(
      gForkId, aId, aOutputData, aOutputSize));
  gChannel->SendMessage(std::move(*msg));
}

void SendRecordingData(size_t aStart, const uint8_t* aData, size_t aSize) {
  MOZ_RELEASE_ASSERT(Thread::CurrentIsMainThread());
  RecordingDataMessage* msg =
      RecordingDataMessage::New(gForkId, aStart, (const char*)aData, aSize);
  gChannel->SendMessage(std::move(*msg));
  free(msg);
}

void SendScanDataToRoot(const char* aData, size_t aSize) {
  MOZ_RELEASE_ASSERT(Thread::CurrentIsMainThread());
  ScanDataMessage* msg = ScanDataMessage::New(gForkId, 0, aData, aSize);
  gChannel->SendMessage(std::move(*msg));
  free(msg);
}

}  // namespace child

void OnWidgetEvent(dom::BrowserChild* aChild, const WidgetEvent& aEvent) {
  if (aEvent.mClass == eMouseEventClass) {
    js::OnMouseEvent(CurrentRecordingTime(), ToChar(aEvent.mMessage),
                     aEvent.mRefPoint.x, aEvent.mRefPoint.y);
  }
}

}  // namespace recordreplay
}  // namespace mozilla
