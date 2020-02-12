/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_recordreplay_ChildInternal_h
#define mozilla_recordreplay_ChildInternal_h

#include "Channel.h"
#include "ChildIPC.h"
#include "JSControl.h"
#include "ExternalCall.h"
#include "Monitor.h"

// This file has internal definitions for communication between the main
// record/replay infrastructure and child side IPC code.

namespace mozilla {
namespace recordreplay {
namespace child {

void SetupRecordReplayChannel(int aArgc, char* aArgv[]);

// Information about a crash that occurred.
struct MinidumpInfo {
  int mExceptionType;
  int mCode;
  int mSubcode;
  mach_port_t mThread;
  mach_port_t mTask;

  MinidumpInfo(int aExceptionType, int aCode, int aSubcode, mach_port_t aThread,
               mach_port_t aTask)
      : mExceptionType(aExceptionType),
        mCode(aCode),
        mSubcode(aSubcode),
        mThread(aThread),
        mTask(aTask) {}
};

void ReportCrash(const MinidumpInfo& aMinidumpInfo, void* aFaultingAddress);

// Generate a minidump and report a fatal error to the middleman process.
void ReportFatalError(const char* aFormat, ...);

// Report an error that will abort the record/replay tab's execution.
void ReportCriticalError(const char* aMessage);

// Report to the middleman that we had an unhandled recording divergence, and
// that execution in this process cannot continue.
void ReportUnhandledDivergence();

// If unhandled divergences are not allowed then we will crash instead of
// reporting them.
void SetUnhandledDivergenceAllowed(bool aAllowed);

// Return whether unhandled divergences are currently allowed.
bool UnhandledDivergenceAllowed();

// Get the unique ID of this child.
size_t GetId();
size_t GetForkId();

// Monitor used for various synchronization tasks.
extern Monitor* gMonitor;

// Notify the middleman that the last manifest was finished.
void ManifestFinished(const js::CharBuffer& aResponse);

// Send messages operating on external calls.
void SendExternalCallRequest(ExternalCallId aId,
                             const char* aInputData, size_t aInputSize,
                             InfallibleVector<char>* aOutputData,
                             bool* aOutputUnavailable);

// Send the output from an external call to the root replaying process,
// to fill in its external call cache.
void SendExternalCallOutput(ExternalCallId aId,
                            const char* aOutputData, size_t aOutputSize);

// Paint according to the current process state, then convert it to an image
// and serialize it in aData.
bool Repaint(nsACString& aData);

// Fork this process and assign a new fork ID to the new process.
void PerformFork(size_t aForkId);

// Send new recording data from a recording process to the middleman.
void SendRecordingData(size_t aStart, const uint8_t* aData, size_t aSize);

void AddPendingRecordingData();

// In a root replaying process, save all recording data to the cloud.
void SaveCloudRecording(const char* aName);

// Set any text to be printed if this process crashes.
void SetCrashNote(const char* aNote);

extern nsCString gReplayJS;

void PrintLog(const nsAString& aText);

}  // namespace child
}  // namespace recordreplay
}  // namespace mozilla

#endif  // mozilla_recordreplay_ChildInternal_h
