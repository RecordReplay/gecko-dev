/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_recordreplay_ParentIPC_h
#define mozilla_recordreplay_ParentIPC_h

#include "base/shared_memory.h"
#include "mozilla/ipc/MessageChannel.h"
#include "mozilla/ipc/ProcessChild.h"
#include "mozilla/ipc/ProtocolUtils.h"
#include "mozilla/ipc/ScopedXREEmbed.h"
#include "mozilla/ipc/Shmem.h"

namespace mozilla {

namespace dom { class ContentParent; }

namespace recordreplay {
namespace parent {

// The middleman process is a content process that manages communication with
// one or more child recording or replaying processes. It performs IPC with the
// UI process in the normal fashion for a content process, using the normal
// IPDL protocols. Communication with a recording or replaying process is done
// via a special IPC channel (see Channel.h), and communication with a
// recording process can additionally be done via IPDL messages, usually by
// forwarding them from the UI process.

// UI process API

// Initialize state in a UI process.
void InitializeUIProcess(int aArgc, char** aArgv);

// Get any directory where content process recordings should be saved.
const char* SaveAllRecordingsDirectory();

// Get the firefox version to include in user agent strings.
const char* CurrentFirefoxVersion();

// Make sure that state in the UI process has been initialized.
void EnsureUIStateInitialized();

// Get the JS that was loaded during initialization.
void GetWebReplayJS(nsAutoCString& aControlJS, nsAutoCString& aReplayJS);

void GetCloudReplayStatus(nsAString& aResult);
void SetCloudReplayStatusCallback(JS::HandleValue aCallback);

double ElapsedTime();

// Middleman process API

// Get the pid of the UI process.
base::ProcessId ParentProcessId();

// Save the recording up to the current point in execution.
void SaveRecording(const ipc::FileDescriptor& aFile);
void SaveCloudRecording(const nsAString& aUUID);

// Get the message channel used to communicate with the UI process.
ipc::MessageChannel* ChannelToUIProcess();

// Post a task to run on the main thread, even if it is blocked waiting to hear
// from a child process.
void DispatchToMainThread(already_AddRefed<nsIRunnable>&& aRunnable);

// Initialize state in a middleman process.
void InitializeMiddleman(int aArgc, char* aArgv[], base::ProcessId aParentPid,
                         const base::SharedMemoryHandle& aPrefsHandle,
                         const ipc::FileDescriptor& aPrefMapHandle);

// Open a socket which a recording/replaying child can use to connect to its
// middleman process.
void OpenChannel(base::ProcessId aMiddlemanPid, uint32_t aChannelId,
                 ipc::FileDescriptor* aConnection);

// Get the command line arguments to use when spawning a recording or replaying
// child process.
void GetArgumentsForChildProcess(base::ProcessId aMiddlemanPid,
                                 uint32_t aChannelId,
                                 const char* aRecordingFile, bool aRecording,
                                 std::vector<std::string>& aExtraArgs);

// Return whether to create replaying processes on a remote machine.
bool UseCloudForReplayingProcesses();

// Create a replaying process on a remote machine for aParent. aChannelId
// is the ID (unique for each middleman) of the resulting channel.
void CreateReplayingCloudProcess(dom::ContentParent* aParent, uint32_t aChannelId);

// Notify when a content parent is being destroyed.
void ContentParentDestroyed(dom::ContentParent* aParent);

// This can be called in parent, middleman, and recording/replaying processes.
void AddToLog(const nsAString& aText, bool aIncludePrefix = true);

// Set the connection status of a cloud replaying status connection.
void SetConnectionStatus(uint32_t aChannelId, const nsCString& aStatus);

}  // namespace parent
}  // namespace recordreplay
}  // namespace mozilla

#endif  // mozilla_recordreplay_ParentIPC_h
