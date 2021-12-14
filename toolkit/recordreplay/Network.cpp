/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Interfaces for wrapping standard request binary response data streams.

#include "ProcessRecordReplay.h"
#include "nsIObserverService.h"
#include "nsIRequest.h"
#include "mozilla/Services.h"

#include "nsCOMPtr.h"
#include "nsComponentManagerUtils.h"
#include "nsCycleCollectionParticipant.h"
#include "nsIStreamListener.h"
#include "nsISupportsImpl.h"
#include "nsIOutputStream.h"
#include "nsIPipe.h"
#include "nsIChannel.h"
#include "nsIInputStreamTee.h"
#include "nsIStreamListenerTee.h"

#include "mozilla/ToString.h"

namespace mozilla::recordreplay {

class ResponseRequestObserver final : public nsIRequestObserver {
 private:
  nsCOMPtr<nsIInputStream> mStream;
  ~ResponseRequestObserver() = default;

 public:
  explicit ResponseRequestObserver(nsIInputStream* aStream): mStream(aStream) {}

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_CLASS(ResponseRequestObserver)

  // nsIRequestObserver
  NS_DECL_NSIREQUESTOBSERVER
};

NS_IMPL_CYCLE_COLLECTION(ResponseRequestObserver, mStream)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(ResponseRequestObserver)
  NS_INTERFACE_MAP_ENTRY(nsIRequestObserver)
NS_INTERFACE_MAP_END

NS_IMPL_CYCLE_COLLECTING_ADDREF(ResponseRequestObserver)
NS_IMPL_CYCLE_COLLECTING_RELEASE(ResponseRequestObserver)

NS_IMETHODIMP
ResponseRequestObserver::OnStartRequest(nsIRequest* request) {
  MOZ_RELEASE_ASSERT(IsRecordingOrReplaying());

  nsCOMPtr<nsIIdentChannel> channel = do_QueryInterface(request);
  nsCOMPtr<nsIObserverService> obsService = mozilla::services::GetObserverService();
  if (channel && obsService) {
    nsAutoString channelIdStr = NS_ConvertUTF8toUTF16(ToString(channel->ChannelId()).c_str());

    obsService->NotifyObservers(mStream, "replay-response-start", channelIdStr.get());
  } else {
    mStream = nullptr;
  }
  return NS_OK;
}

NS_IMETHODIMP
ResponseRequestObserver::OnStopRequest(nsIRequest* request, nsresult status) {
  return NS_OK;
}

already_AddRefed<nsIStreamListener> WrapNetworkStreamListener(nsIStreamListener* listener) {
  if (!IsRecordingOrReplaying()) {
    return nsCOMPtr(listener).forget();
  }

  nsCOMPtr<nsIOutputStream> outputStream;
  nsCOMPtr<nsIInputStream> inputStream;
  nsresult rv = NS_NewPipe(
      getter_AddRefs(inputStream), getter_AddRefs(outputStream),
      0, UINT32_MAX,
      true, false);
  MOZ_ALWAYS_SUCCEEDS(rv);

  nsCOMPtr<nsIRequestObserver> observer = new ResponseRequestObserver(inputStream);

  nsCOMPtr<nsIStreamListenerTee> tee = do_CreateInstance("@mozilla.org/network/stream-listener-tee;1");
  rv = tee->Init(listener, outputStream, observer);
  MOZ_ALWAYS_SUCCEEDS(rv);

  return tee.forget();
}

} // namespace mozilla::recordreplay
