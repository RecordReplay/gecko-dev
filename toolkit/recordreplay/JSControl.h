/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_recordreplay_JSControl_h
#define mozilla_recordreplay_JSControl_h

#include "jsapi.h"

#include "InfallibleVector.h"

#include "mozilla/DefineEnum.h"
#include "nsString.h"

namespace mozilla {
namespace recordreplay {

struct Message;

namespace parent {
class ChildProcessInfo;
}

namespace js {

void InitializeJS();
void EnsureModuleInitialized();

// Notify the UI process that the recording was finished.
void SendRecordingFinished();

// Make sure the UI process is notified if the recording is unusable.
void MaybeSendRecordingUnusable();

}  // namespace js
}  // namespace recordreplay
}  // namespace mozilla

#endif  // mozilla_recordreplay_JSControl_h
