/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_recordreplay_ParentInternal_h
#define mozilla_recordreplay_ParentInternal_h

#include "ParentIPC.h"

#include "Channel.h"
#include "mozilla/ipc/GeckoChildProcessHost.h"

namespace mozilla {
namespace recordreplay {

// Used in parent/middleman processes for the main thread's event loop,
// and in replaying processes while debugging.
void BeginRunEvent(const TimeStamp& aNow);
void EndRunEvent();

namespace parent {

// This file has internal declarations for interaction between different
// components of middleman logic.

class ChildProcessInfo;

// Get the message loop for the main thread.
MessageLoop* MainThreadMessageLoop();

// Immediately forward any parent->child or sync child->parent IPDL messages.
// These are sent on the main thread, which might be blocked waiting for a
// response from the recording child and unable to run an event loop.
void MaybeHandleForwardedMessages();

// Initialize state which handles incoming IPDL messages from the UI and
// recording child processes.
void InitializeForwarding();

// Terminate all children and kill this process.
void Shutdown();

// Monitor used for synchronizing between the main and channel or message loop
// threads.
extern Monitor* gMonitor;

///////////////////////////////////////////////////////////////////////////////
// Graphics
///////////////////////////////////////////////////////////////////////////////

// Painting can happen in two ways:
//
// - When the main child runs (the recording child, or a dedicated replaying
//   child if there is no recording child), it does so on the user's machine and
//   paints into gGraphicsMemory, a buffer shared with the middleman process.
//   After the buffer has been updated, a PaintMessage is sent to the middleman.
//
// - When the user is within the recording and we want to repaint old graphics,
//   gGraphicsMemory is not updated (the replaying process could be on a distant
//   machine and be unable to access the buffer). Instead, the replaying process
//   does its repaint locally, losslessly compresses it to a PNG image, encodes
//   it to base64, and sends it to the middleman. The middleman then undoes this
//   encoding and paints the resulting image.
//
// In either case, a canvas in the middleman is filled with the paint data,
// updating the graphics shown by the UI process. The canvas is managed by
// devtools/server/actors/replay/graphics.js
extern void* gGraphicsMemory;

void InitializeGraphicsMemory();
void SendGraphicsMemoryToChild();

// Update the graphics painted in the UI process after a paint happened in the
// main child.
void UpdateGraphicsAfterPaint(const PaintMessage& aMsg);

// Update the graphics painted in the UI process according to some paint data.
void PaintGraphics(const nsACString& aMimeType, const nsACString& aImageData,
                   const nsACString& aOptions);

// Clear any graphics painted in the UI process.
void ClearGraphics(const nsACString& aOptions);

// Restore the graphics last painted by the main child.
void RestoreMainGraphics();

// Restore the listener for suppressed events on the canvas.
void RestoreSuppressedEventListener();

// ID for the mach message sent from a child process to the middleman to
// request a port for the graphics shmem.
static const int32_t GraphicsHandshakeMessageId = 42;

// ID for the mach message sent from the middleman to a child process with the
// requested memory for.
static const int32_t GraphicsMemoryMessageId = 43;

// Fixed size of the graphics shared memory buffer.
static const size_t GraphicsMemorySize = 4096 * 4096 * 4;

// Return whether the environment variable activating repaint stress mode is
// set. This makes various changes in both the middleman and child processes to
// trigger a child to diverge from the recording and repaint on every vsync,
// making sure that repainting can handle all the system interactions that
// occur while painting the current tab.
bool InRepaintStressMode();

///////////////////////////////////////////////////////////////////////////////
// Child Processes
///////////////////////////////////////////////////////////////////////////////

// Handle to the underlying recording process, if there is one. Recording
// processes are directly spawned by the middleman at startup, since they need
// to receive all the same IPC which the middleman receives from the UI process
// in order to initialize themselves. Replaying processes are all spawned by
// the UI process itself, due to sandboxing restrictions.
extern ipc::GeckoChildProcessHost* gRecordingProcess;

// Any information needed to spawn a recording child process, in addition to
// the contents of the introduction message.
struct RecordingProcessData {
  // File descriptors that will need to be remapped for the child process.
  const base::SharedMemoryHandle& mPrefsHandle;
  const ipc::FileDescriptor& mPrefMapHandle;

  RecordingProcessData(const base::SharedMemoryHandle& aPrefsHandle,
                       const ipc::FileDescriptor& aPrefMapHandle)
      : mPrefsHandle(aPrefsHandle), mPrefMapHandle(aPrefMapHandle) {}
};

// Information about a recording or replaying child process.
class ChildProcessInfo {
  // Channel for communicating with the process.
  Channel* mChannel = nullptr;

  // Whether this process is recording.
  bool mRecording = false;

  void OnIncomingMessage(const Message& aMsg, double aDelay);

  static void MaybeProcessPendingMessageRunnable();
  static void ReceiveChildMessageOnMainThread(size_t aChildId,
                                              Message::UniquePtr aMsg);

  void OnCrash(size_t aForkId, const char* aWhy);
  void LaunchSubprocess(
      size_t aId, const Maybe<RecordingProcessData>& aRecordingProcessData,
      size_t aInitialReplayingLength);

 public:
  explicit ChildProcessInfo(
      size_t aId, const Maybe<RecordingProcessData>& aRecordingProcessData,
      size_t aInitialReplayingLength);
  ~ChildProcessInfo();

  size_t GetId() { return mChannel->GetId(); }
  bool IsRecording() { return mRecording; }

  // Send a message over the underlying channel.
  void SendMessage(Message&& aMessage);

  static void SetIntroductionMessage(IntroductionMessage* aMessage);
  static void MaybeProcessNextMessage();
};

extern ChildProcessInfo* gRecordingChild;

}  // namespace parent
}  // namespace recordreplay
}  // namespace mozilla

#endif  // mozilla_recordreplay_ParentInternal_h
