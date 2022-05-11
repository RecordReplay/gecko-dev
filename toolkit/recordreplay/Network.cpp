/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Interfaces for wrapping standard request binary response data streams.

#include "ProcessRecordReplay.h"
#include "nsIObserverService.h"
#include "nsIRequest.h"
#include "mozilla/InputStreamLengthHelper.h"
#include "mozilla/InputStreamLengthWrapper.h"
#include "mozilla/Services.h"
#include "mozilla/ToString.h"

#include "nsCOMPtr.h"
#include "nsComponentManagerUtils.h"
#include "nsCycleCollectionParticipant.h"
#include "nsNetUtil.h"
#include "nsIAsyncInputStream.h"
#include "nsIChannel.h"
#include "nsIHttpChannel.h"
#include "nsIInputStream.h"
#include "nsIInputStreamLength.h"
#include "nsIOutputStream.h"
#include "nsIPipe.h"
#include "nsIStreamListener.h"
#include "nsIStreamListenerTee.h"
#include "nsISupportsImpl.h"

namespace mozilla {
extern std::string NumberToStringRecordReplayWorkaroundForWindows(uint64_t v);
}

namespace mozilla::recordreplay {

// Define a custom class for listening for request start so that we can
// be notified when the request begins.
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
    nsAutoString channelIdStr = NS_ConvertUTF8toUTF16(
        NumberToStringRecordReplayWorkaroundForWindows(channel->ChannelId()).c_str());

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

/**
 * The gecko-standard nsInputStreamTee implementation you get from 'NS_NewInputStreamTee'
 * only implements 'nsIInputStream', but for things to behave well when wrapping network
 * request bodies, we need to implement 'nsIAsyncInputStream' and 'nsIInputStreamLength'
 * at a minimum.
 *
 * NOTE: Much of the logic here is copied and tweaked from other stream implementations.
 */
class ReplayInputStreamTee final : public nsIAsyncInputStream,
                                   public nsIInputStreamLength,
                                   public nsIInputStreamCallback,
                                   public nsIAsyncInputStreamLength,
                                   public nsIInputStreamLengthCallback {
 public:
  ReplayInputStreamTee(nsIInputStream* aStream, nsIOutputStream* aSink);

  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIINPUTSTREAM
  NS_DECL_NSIASYNCINPUTSTREAM
  NS_DECL_NSIINPUTSTREAMLENGTH
  NS_DECL_NSIINPUTSTREAMCALLBACK
  NS_DECL_NSIASYNCINPUTSTREAMLENGTH
  NS_DECL_NSIINPUTSTREAMLENGTHCALLBACK

 private:
  nsCOMPtr<nsIInputStream> mSource;
  nsCOMPtr<nsIOutputStream> mSink;
  ~ReplayInputStreamTee() = default;

  // Optional interfaces implemented by source.
  nsCOMPtr<nsIAsyncInputStream> mAsyncSource;
  nsCOMPtr<nsIInputStreamLength> mSourceLength;
  nsCOMPtr<nsIAsyncInputStreamLength> mAsyncSourceLength;

  // This is used for AsyncWait and it's protected by mutex.
  mozilla::Mutex mCallbackMutex;
  nsCOMPtr<nsIInputStreamCallback> mAsyncWaitCallback;
  nsCOMPtr<nsIInputStreamLengthCallback> mAsyncLengthWaitCallback;
};

NS_IMPL_ADDREF(ReplayInputStreamTee)
NS_IMPL_RELEASE(ReplayInputStreamTee)
NS_INTERFACE_MAP_BEGIN(ReplayInputStreamTee)
  NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIAsyncInputStream)
  NS_INTERFACE_MAP_ENTRY(nsIInputStream)
  NS_INTERFACE_MAP_ENTRY_CONDITIONAL(nsIAsyncInputStream, mAsyncSource)
  NS_INTERFACE_MAP_ENTRY(nsIInputStreamCallback)
  NS_INTERFACE_MAP_ENTRY_CONDITIONAL(nsIInputStreamLength, mSourceLength)
  NS_INTERFACE_MAP_ENTRY_CONDITIONAL(nsIAsyncInputStreamLength, mAsyncSourceLength)
  NS_INTERFACE_MAP_ENTRY(nsIInputStreamLengthCallback)
NS_INTERFACE_MAP_END


ReplayInputStreamTee::ReplayInputStreamTee(nsIInputStream* aSource, nsIOutputStream* aSink)
    : mSource(aSource), mSink(aSink), mCallbackMutex("ReplayInputStreamTee::mCallbackMutex") {
  // NOTE: Not sure if the COMIdentity check here is necessary, but I saw it
  // in a few other wrapper stream implementations so it seemed like a good idea.
  nsCOMPtr<nsIAsyncInputStream> asyncSource(do_QueryInterface(mSource));
  if (SameCOMIdentity(mSource, asyncSource)) {
    mAsyncSource = asyncSource;
  }
  nsCOMPtr<nsIInputStreamLength> sourceLength(do_QueryInterface(mSource));
  if (SameCOMIdentity(mSource, sourceLength)) {
    mSourceLength = sourceLength;
  }
  nsCOMPtr<nsIAsyncInputStreamLength> asyncSourceLength(do_QueryInterface(mSource));
  if (SameCOMIdentity(mSource, asyncSourceLength)) {
    mAsyncSourceLength = asyncSourceLength;
  }
}

// nsIInputStream

NS_IMETHODIMP
ReplayInputStreamTee::Close() {
  nsresult rv = mSource->Close();
  if (mSink) {
    nsresult rv2 = mSink->Close();
    mSink = nullptr;
    if (NS_FAILED(rv2)) {
      if (NS_SUCCEEDED(rv)) {
        rv = rv2;
      } else {
        NS_WARNING("ReplayInputStreamTee close silently failed");
      }
    }
  }
  return rv;
}

NS_IMETHODIMP
ReplayInputStreamTee::Available(uint64_t* aResult) {
  nsresult rv = mSource->Available(aResult);

  if (rv == NS_BASE_STREAM_CLOSED && mSink) {
    nsresult rv2 = mSink->Close();
    mSink = nullptr;
    if (NS_FAILED(rv2)) {
      if (NS_SUCCEEDED(rv)) {
        rv = rv2;
      } else {
        NS_WARNING("ReplayInputStreamTee close silently failed");
      }
    }
  }

  return rv;
}

NS_IMETHODIMP
ReplayInputStreamTee::Read(char* aBuf, uint32_t aCount, uint32_t* aResult) {
  nsresult rv = mSource->Read(aBuf, aCount, aResult);

  if (NS_FAILED(rv)) {
    return rv;
  }

  if (*aResult == 0 && mSink) {
    rv = mSink->Close();
    mSink = nullptr;
    return rv;
  }

  if (!mSink) {
    return NS_OK;
  }

  uint32_t remaining = *aResult;
  uint32_t totalBytesWritten = 0;
  while (remaining > 0) {
    uint32_t bytesWritten = 0;
    rv = mSink->Write(aBuf + totalBytesWritten, remaining, &bytesWritten);

    if (NS_FAILED(rv)) {
      NS_WARNING("ReplayInputStreamTee write silently failed, dropping sink");
      mSink = nullptr;
      break;
    }
    NS_ASSERTION(bytesWritten <= remaining, "wrote too much");
    totalBytesWritten += bytesWritten;
    remaining -= bytesWritten;
  }

  return NS_OK;
}

NS_IMETHODIMP
ReplayInputStreamTee::ReadSegments(nsWriteSegmentFun aWriter, void* aClosure,
                                   uint32_t aCount, uint32_t* aResult) {
  // The source stream probably has some buffering, and we're immediately
  // wrapping this stream in a buffered stream anyway, so leaving this
  // un-implemented keeps things simpler. Wrappers will just call Read() instead.
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
ReplayInputStreamTee::IsNonBlocking(bool* aNonBlocking) {
  return mSource->IsNonBlocking(aNonBlocking);
}

// nsIAsyncInputStream

NS_IMETHODIMP
ReplayInputStreamTee::AsyncWait(nsIInputStreamCallback* aCallback, uint32_t aFlags,
                                uint32_t aRequestedCount, nsIEventTarget* aTarget) {
  MOZ_ASSERT(mAsyncSource);

  {
    MutexAutoLock lock(mCallbackMutex);

    if (mAsyncWaitCallback && aCallback) {
      // Cannot wait more than once.
      return NS_ERROR_FAILURE;
    }

    if (!mAsyncWaitCallback && !aCallback) {
      // Nothing to do.
      return NS_OK;
    }

    mAsyncWaitCallback = aCallback;
  }

  nsCOMPtr<nsIInputStreamCallback> callback = aCallback ? this : nullptr;

  return mAsyncSource->AsyncWait(callback, aFlags, aRequestedCount,
                                          aTarget);
}

NS_IMETHODIMP
ReplayInputStreamTee::CloseWithStatus(nsresult aRv) {
  MOZ_ASSERT(mAsyncSource);
  nsresult rv = mAsyncSource->CloseWithStatus(aRv);
  if (mSink) {
    nsresult rv2 = mSink->Close();
    mSink = nullptr;
    if (NS_FAILED(rv2)) {
      if (NS_SUCCEEDED(rv)) {
        rv = rv2;
      } else {
        NS_WARNING("ReplayInputStreamTee closewithstatus silently failed");
      }
    }
  }
  return rv;
}

// nsIInputStreamCallback

NS_IMETHODIMP
ReplayInputStreamTee::OnInputStreamReady(nsIAsyncInputStream* aStream) {
  MOZ_ASSERT(mAsyncSource);
  MOZ_ASSERT(mAsyncSource == aStream);

  nsCOMPtr<nsIInputStreamCallback> callback;
  {
    MutexAutoLock lock(mCallbackMutex);
    callback.swap(mAsyncWaitCallback);
  }

  // Something could have removed the callback while we were waiting.
  if (!callback) {
    return NS_OK;
  }

  MOZ_ASSERT(callback);
  return callback->OnInputStreamReady(this);
}

// nsIInputStreamLength

NS_IMETHODIMP
ReplayInputStreamTee::Length(int64_t* aLength) {
  MOZ_ASSERT(mSourceLength);
  return mSourceLength->Length(aLength);
}


// nsIAsyncInputStreamLength

NS_IMETHODIMP
ReplayInputStreamTee::AsyncLengthWait(nsIInputStreamLengthCallback* aCallback,
                                      nsIEventTarget* aEventTarget) {
  MOZ_ASSERT(mAsyncSourceLength);

  {
    MutexAutoLock lock(mCallbackMutex);

    if (mAsyncLengthWaitCallback && aCallback) {
      // Cannot wait more than once.
      return NS_ERROR_FAILURE;
    }

    if (!mAsyncLengthWaitCallback && !aCallback) {
      // Nothing to do.
      return NS_OK;
    }

    mAsyncLengthWaitCallback = aCallback;
  }

  nsCOMPtr<nsIInputStreamLengthCallback> callback = aCallback ? this : nullptr;

  return mAsyncSourceLength->AsyncLengthWait(callback, aEventTarget);
}


// nsIInputStreamLengthCallback

NS_IMETHODIMP
ReplayInputStreamTee::OnInputStreamLengthReady(nsIAsyncInputStreamLength* aStream,
                                               int64_t aLength) {
  MOZ_ASSERT(mAsyncSourceLength);
  MOZ_ASSERT(mAsyncSourceLength == aStream);

  nsCOMPtr<nsIInputStreamLengthCallback> callback;
  {
    MutexAutoLock lock(mCallbackMutex);
    callback.swap(mAsyncLengthWaitCallback);
  }

  // Something could have removed the callback while we were waiting.
  if (!callback) {
    return NS_OK;
  }

  return callback->OnInputStreamLengthReady(this, aLength);
}


/**
 * The output of this wrapper is an nsIInputStream that will be sent to the
 * parent process so that the parent can send the request. The input stream
 * passed into this function generally implements
 * nsIIPCSerializableInputStream, so they can be fully serialized and sent to
 * the parent without actually using the nsIInputStream API in the child.
 * Since we have to observe the body while it is in transit, we do not implement
 * that interface. We do however need to implement nsIAsyncInputStream,
 * nsIInputStreamLength, and nsIAsyncInputStreamLength in order to ensure that
 * the stream we return can be properly consumed and sent to the parent as
 * individual chunks.
 */
already_AddRefed<nsIInputStream> WrapNetworkRequestBodyStream(nsIHttpChannel* aChannel,
                                                              nsIInputStream* aStream,
                                                              int64_t aLength) {
  nsCOMPtr stream(aStream);

  if (!IsRecordingOrReplaying()) {
    return stream.forget();
  }

  nsCOMPtr<nsIIdentChannel> channel = do_QueryInterface(aChannel);
  nsCOMPtr<nsIObserverService> obsService = mozilla::services::GetObserverService();
  if (!channel || !obsService) {
    return stream.forget();
  }

  int64_t streamLength;
  if (InputStreamLengthHelper::GetSyncLength(aStream, &streamLength) && streamLength >= 0) {
    // Existing implementation of length is fine.
  } else if (aLength >= 0) {
    stream = new InputStreamLengthWrapper(stream.forget(), aLength);
  } else {
    // It appears that currently the HTTPChannelParent relies on knowing the length of
    // the upload body before the upload begins, but if the length is not known here,
    // it will have no way to do that, and it fails. This is because
    // InputStreamLengthHelper::GetAsyncLength requires the upload stream to
    // implement nsIAsyncInputStreamLength, and because we are wrapping the response
    // body in a pipe to send it to the child, the parent doesn't implement it.
    return stream.forget();
  }

  nsCOMPtr<nsIOutputStream> outputStream;
  nsCOMPtr<nsIInputStream> inputStream;
  nsresult rv = NS_NewPipe(
      getter_AddRefs(inputStream), getter_AddRefs(outputStream),
      0, UINT32_MAX,
      true, false);
  MOZ_ALWAYS_SUCCEEDS(rv);

  // Tee the upload stream into the new pipe so we can pass the pipe off to our
  // JS code to read from.
  nsCOMPtr<nsIInputStream> tee(new ReplayInputStreamTee(stream, outputStream));

  nsAutoString channelIdStr = NS_ConvertUTF8toUTF16(
      NumberToStringRecordReplayWorkaroundForWindows(channel->ChannelId()).c_str());
  obsService->NotifyObservers(inputStream, "replay-request-start", channelIdStr.get());

  // Buffering is necessarily because ExplicitSetUploadStream errors if the
  // stream is not buffered.
  // Additionally, even if the source stream is buffered, we want to explicitly
  // use a large buffer so that we can allow data to flow to the parent as
  // quickly as possible. Adding this tee wrapper causes the HTTP upload
  // logic to switch from serializing the stream as a single unit
  // to serializing as a pipe and then filling the pipe, so we want to
  // fill that pipe as quickly as possible.
  // Node uses 64k for its in-memory file buffering, so doing the same value.
  nsCOMPtr<nsIInputStream> bufferedTee;
  rv = NS_NewBufferedInputStream(getter_AddRefs(bufferedTee),
                                  tee.forget(), 65536);
  MOZ_ALWAYS_SUCCEEDS(rv);

  return bufferedTee.forget();
}

} // namespace mozilla::recordreplay
