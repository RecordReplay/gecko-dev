/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/PreallocatedProcessManager.h"

#include "mozilla/ClearOnShutdown.h"
#include "mozilla/Preferences.h"
#include "mozilla/ProfilerMarkers.h"
#include "mozilla/Unused.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/ScriptSettings.h"
#include "mozilla/StaticPrefs_dom.h"
#include "nsIPropertyBag2.h"
#include "ProcessPriorityManager.h"
#include "nsServiceManagerUtils.h"
#include "nsIXULRuntime.h"
#include <deque>

using namespace mozilla::hal;
using namespace mozilla::dom;

namespace mozilla {
/**
 * This singleton class implements the static methods on
 * PreallocatedProcessManager.
 */
class PreallocatedProcessManagerImpl final : public nsIObserver {
  friend class PreallocatedProcessManager;

 public:
  static PreallocatedProcessManagerImpl* Singleton();
  static PreallocatedProcessManagerImpl* SingletonForRecording();

  NS_DECL_ISUPPORTS
  NS_DECL_NSIOBSERVER

  // See comments on PreallocatedProcessManager for these methods.
  static void AddBlocker();
  static void RemoveBlocker();
  already_AddRefed<ContentParent> Take(const nsACString& aRemoteType);
  bool Erase(ContentParent* aParent);

 private:
  static const char* const kObserverTopics[];

  static StaticRefPtr<PreallocatedProcessManagerImpl> sSingleton;
  static StaticRefPtr<PreallocatedProcessManagerImpl> sSingletonForRecording;

  static nsresult GetReplayDispatchServer(nsAString& dispatchServer);

  PreallocatedProcessManagerImpl();
  PreallocatedProcessManagerImpl(const nsAString& aRecordingDispatchAddress);
  ~PreallocatedProcessManagerImpl();
  PreallocatedProcessManagerImpl(const PreallocatedProcessManagerImpl&) =
      delete;

  const PreallocatedProcessManagerImpl& operator=(
      const PreallocatedProcessManagerImpl&) = delete;

  void Init();

  bool CanAllocate();
  void AllocateAfterDelay();
  void AllocateOnIdle();
  void AllocateNow();

  void RereadPrefs();
  void Enable(uint32_t aProcesses);
  void Disable();
  void CloseProcesses();


  bool IsEmpty() const {
    return mPreallocatedProcesses.empty() && !mLaunchInProgress;
  }

  void StartBlockers();
  void EndBlockers();

  bool mEnabled;
  static bool sShutdown;
  bool mLaunchInProgress;
  uint32_t mNumberPreallocs;
  nsString mRecordingDispatchAddress;
  nsString mRecordingUserToken;
  std::deque<RefPtr<ContentParent>> mPreallocatedProcesses;
  // Even if we have multiple PreallocatedProcessManagerImpls, we'll have
  // one blocker counter
  static uint32_t sNumBlockers;
  TimeStamp mBlockingStartTime;
};

/* static */
uint32_t PreallocatedProcessManagerImpl::sNumBlockers = 0;
bool PreallocatedProcessManagerImpl::sShutdown = false;

const char* const PreallocatedProcessManagerImpl::kObserverTopics[] = {
    "memory-pressure",
    "profile-change-teardown",
    NS_XPCOM_SHUTDOWN_OBSERVER_ID,
};

/* static */
StaticRefPtr<PreallocatedProcessManagerImpl>
    PreallocatedProcessManagerImpl::sSingleton;

/* static */
StaticRefPtr<PreallocatedProcessManagerImpl>
    PreallocatedProcessManagerImpl::sSingletonForRecording;

/* static */
PreallocatedProcessManagerImpl* PreallocatedProcessManagerImpl::Singleton() {
  MOZ_ASSERT(NS_IsMainThread());
  if (!sSingleton) {
    sSingleton = new PreallocatedProcessManagerImpl;
    sSingleton->Init();
    ClearOnShutdown(&sSingleton);

    // Attempt to create a preallocator for recorded children exactly
    // once, when the main preallocator singleton is created.
    MOZ_ASSERT(!sSingletonForRecording);
    nsString replayServerAddr;
    nsresult rv = GetReplayDispatchServer(replayServerAddr);
    if (!NS_FAILED(rv) && replayServerAddr.Length() > 0) {
      sSingletonForRecording = new PreallocatedProcessManagerImpl(replayServerAddr);
      sSingletonForRecording->Init();
      ClearOnShutdown(&sSingletonForRecording);
    }
  }
  return sSingleton;
  //  PreallocatedProcessManagers live until shutdown
}

/* static */
PreallocatedProcessManagerImpl*
PreallocatedProcessManagerImpl::SingletonForRecording() {
  MOZ_ASSERT(NS_IsMainThread());
  Singleton();
  return sSingletonForRecording;
}

NS_IMPL_ISUPPORTS(PreallocatedProcessManagerImpl, nsIObserver)

PreallocatedProcessManagerImpl::PreallocatedProcessManagerImpl()
    : mEnabled(false), mLaunchInProgress(false), mNumberPreallocs(1),
      mRecordingDispatchAddress() {}
PreallocatedProcessManagerImpl::PreallocatedProcessManagerImpl(
    const nsAString& aRecordingDispatchAddress)
    : mEnabled(false), mLaunchInProgress(false), mNumberPreallocs(1),
      mRecordingDispatchAddress(aRecordingDispatchAddress) {}

PreallocatedProcessManagerImpl::~PreallocatedProcessManagerImpl() {
  // This shouldn't happen, because the promise callbacks should
  // hold strong references, but let't make absolutely sure:
  MOZ_RELEASE_ASSERT(!mLaunchInProgress);
}

nsresult PreallocatedProcessManagerImpl::GetReplayDispatchServer(nsAString& addr) {
  const char* envAddr = getenv("RECORD_REPLAY_SERVER");
  if (envAddr) {
    CopyUTF8toUTF16(mozilla::Span(envAddr, strlen(envAddr)), addr);
    return NS_OK;
  }
  return Preferences::GetString("devtools.recordreplay.cloudServer", addr);
}

void PreallocatedProcessManagerImpl::Init() {
  Preferences::AddStrongObserver(this, "dom.ipc.processPrelaunch.enabled");
  // We have to respect processCount at all time. This is especially important
  // for testing.
  Preferences::AddStrongObserver(this, "dom.ipc.processCount");
  // A StaticPref, but we need to adjust the number of preallocated processes
  // if the value goes up or down, so we need to run code on change.
  Preferences::AddStrongObserver(this,
                                 "dom.ipc.processPrelaunch.fission.number");

  if (mRecordingDispatchAddress.Length() > 0) {
    Preferences::GetString("devtools.recordreplay.user-token", mRecordingUserToken);
    Preferences::AddStrongObserver(this, "devtools.recordreplay.user-token");
  }

  nsCOMPtr<nsIObserverService> os = services::GetObserverService();
  MOZ_ASSERT(os);
  for (auto topic : kObserverTopics) {
    os->AddObserver(this, topic, /* ownsWeak */ false);
  }
  RereadPrefs();
}

NS_IMETHODIMP
PreallocatedProcessManagerImpl::Observe(nsISupports* aSubject,
                                        const char* aTopic,
                                        const char16_t* aData) {
  if (!strcmp("nsPref:changed", aTopic)) {
    // The only other observer we registered was for our prefs.
    RereadPrefs();
  } else if (!strcmp(NS_XPCOM_SHUTDOWN_OBSERVER_ID, aTopic) ||
             !strcmp("profile-change-teardown", aTopic)) {
    Preferences::RemoveObserver(this, "dom.ipc.processPrelaunch.enabled");
    Preferences::RemoveObserver(this, "dom.ipc.processCount");
    Preferences::RemoveObserver(this,
                                "dom.ipc.processPrelaunch.fission.number");

    nsCOMPtr<nsIObserverService> os = services::GetObserverService();
    MOZ_ASSERT(os);
    for (auto topic : kObserverTopics) {
      os->RemoveObserver(this, topic);
    }
    // Let's prevent any new preallocated processes from starting. ContentParent
    // will handle the shutdown of the existing process and the
    // mPreallocatedProcesses reference will be cleared by the ClearOnShutdown
    // of the manager singleton.
    sShutdown = true;
  } else if (!strcmp("memory-pressure", aTopic)) {
    CloseProcesses();
  } else {
    MOZ_ASSERT_UNREACHABLE("Unknown topic");
  }

  return NS_OK;
}

void PreallocatedProcessManagerImpl::RereadPrefs() {
  if (mRecordingDispatchAddress.Length() > 0) {
    nsString userToken;
    Preferences::GetString("devtools.recordreplay.user-token", userToken);
    if (!userToken.Equals(mRecordingUserToken)) {
      mRecordingUserToken = userToken;
      CloseProcesses();
      AllocateOnIdle();
    }
  }
  if (mozilla::BrowserTabsRemoteAutostart() &&
      Preferences::GetBool("dom.ipc.processPrelaunch.enabled")) {
    int32_t number = 1;
    if (mozilla::FissionAutostart()) {
      number = StaticPrefs::dom_ipc_processPrelaunch_fission_number();
    }
    if (number >= 0) {
      Enable(number);
      // We have one prealloc queue for all types except File now
      if (static_cast<uint64_t>(number) < mPreallocatedProcesses.size()) {
        CloseProcesses();
      }
    }
  } else {
    Disable();
  }
}

already_AddRefed<ContentParent> PreallocatedProcessManagerImpl::Take(
    const nsACString& aRemoteType) {
  if (!mEnabled || sShutdown) {
    return nullptr;
  }
  RefPtr<ContentParent> process;
  if (!mPreallocatedProcesses.empty()) {
    process = mPreallocatedProcesses.front().forget();
    mPreallocatedProcesses.pop_front();  // holds a nullptr

    ProcessPriorityManager::SetProcessPriority(process,
                                               PROCESS_PRIORITY_FOREGROUND);

    // We took a preallocated process. Let's try to start up a new one
    // soon.
    AllocateOnIdle();
    MOZ_LOG(ContentParent::GetLog(), LogLevel::Debug,
            ("Use prealloc process %p", process.get()));
  }
  return process.forget();
}

bool PreallocatedProcessManagerImpl::Erase(ContentParent* aParent) {
  // Ensure this ContentParent isn't cached
  for (auto it = mPreallocatedProcesses.begin();
       it != mPreallocatedProcesses.end(); it++) {
    if (*it == aParent) {
      mPreallocatedProcesses.erase(it);
      return true;
    }
  }
  return false;
}

void PreallocatedProcessManagerImpl::Enable(uint32_t aProcesses) {
  mNumberPreallocs = aProcesses;
  if (mEnabled) {
    return;
  }

  mEnabled = true;
  AllocateAfterDelay();
}

/* static */
void PreallocatedProcessManagerImpl::AddBlocker() {
  if (sNumBlockers == 0) {
    if (auto* impl = Singleton()) {
      impl->StartBlockers();
    }
    if (auto* implRec = SingletonForRecording()) {
      implRec->StartBlockers();
    }
  }
  sNumBlockers++;
}

/* static */
void PreallocatedProcessManagerImpl::RemoveBlocker() {
  // This used to assert that the blocker existed, but preallocated
  // processes aren't blockers anymore because it's not useful and
  // interferes with async launch, and it's simpler if content
  // processes don't need to remember whether they were preallocated.

  MOZ_DIAGNOSTIC_ASSERT(sNumBlockers > 0);
  sNumBlockers--;
  if (sNumBlockers == 0) {
    if (auto* impl = Singleton()) {
      impl->EndBlockers();
    }
    if (auto* implRec = SingletonForRecording()) {
      implRec->EndBlockers();
    }
  }
}

void PreallocatedProcessManagerImpl::StartBlockers() {
  mBlockingStartTime = TimeStamp::Now();
}
void PreallocatedProcessManagerImpl::EndBlockers() {
  MOZ_LOG(ContentParent::GetLog(), LogLevel::Debug,
          ("Blocked preallocation for %fms",
           (TimeStamp::Now() - mBlockingStartTime).ToMilliseconds()));
  PROFILER_MARKER_TEXT("Process", DOM,
                       MarkerTiming::IntervalUntilNowFrom(mBlockingStartTime),
                       "Blocked preallocation");
  if (IsEmpty()) {
    AllocateAfterDelay();
  }
}
bool PreallocatedProcessManagerImpl::CanAllocate() {
  return mEnabled && sNumBlockers == 0 &&
         mPreallocatedProcesses.size() < mNumberPreallocs && !sShutdown &&
         (FissionAutostart() ||
          !ContentParent::IsMaxProcessCountReached(DEFAULT_REMOTE_TYPE));
}

void PreallocatedProcessManagerImpl::AllocateAfterDelay() {
  if (!mEnabled) {
    return;
  }

  NS_DelayedDispatchToCurrentThread(
      NewRunnableMethod("PreallocatedProcessManagerImpl::AllocateOnIdle", this,
                        &PreallocatedProcessManagerImpl::AllocateOnIdle),
      StaticPrefs::dom_ipc_processPrelaunch_delayMs());
}

void PreallocatedProcessManagerImpl::AllocateOnIdle() {
  if (!mEnabled) {
    return;
  }

  NS_DispatchToCurrentThreadQueue(
      NewRunnableMethod("PreallocatedProcessManagerImpl::AllocateNow", this,
                        &PreallocatedProcessManagerImpl::AllocateNow),
      EventQueuePriority::Idle);
}

void PreallocatedProcessManagerImpl::AllocateNow() {
  if (!CanAllocate()) {
    if (mEnabled && !sShutdown && IsEmpty() && sNumBlockers > 0) {
      // If it's too early to allocate a process let's retry later.
      AllocateAfterDelay();
    }
    return;
  }

  RefPtr<PreallocatedProcessManagerImpl> self(this);
  mLaunchInProgress = true;

  ContentParent::PreallocateProcess(mRecordingDispatchAddress)->Then(
      GetCurrentSerialEventTarget(), __func__,

      [self, this](const RefPtr<ContentParent>& process) {
        mLaunchInProgress = false;
        if (process->IsDead()) {
          // Process died in startup (before we could add it).  If it
          // dies after this, MarkAsDead() will Erase() this entry.
          // Shouldn't be in the sBrowserContentParents, so we don't need
          // RemoveFromList().  We won't try to kick off a new
          // preallocation here, to avoid possible looping if something is
          // causing them to consistently fail; if everything is ok on the
          // next allocation request we'll kick off creation.
        } else {
          if (CanAllocate()) {
            // slight perf reason for push_back - while the cpu cache
            // probably has stack/etc associated with the most recent
            // process created, we don't know that it has finished startup.
            // If we added it to the queue on completion of startup, we
            // could push_front it, but that would require a bunch more
            // logic.
            mPreallocatedProcesses.push_back(process);
            MOZ_LOG(ContentParent::GetLog(), LogLevel::Debug,
                    ("Preallocated = %lu of %d processes",
                     (unsigned long)mPreallocatedProcesses.size(),
                     mNumberPreallocs));

            // Continue prestarting processes if needed
            if (mPreallocatedProcesses.size() < mNumberPreallocs) {
              AllocateOnIdle();
            }
          } else {
            process->ShutDownProcess(ContentParent::SEND_SHUTDOWN_MESSAGE);
          }
        }
      },

      [self, this](ContentParent::LaunchError err) {
        mLaunchInProgress = false;
      });
}

void PreallocatedProcessManagerImpl::Disable() {
  if (!mEnabled) {
    return;
  }

  mEnabled = false;
  CloseProcesses();
}

void PreallocatedProcessManagerImpl::CloseProcesses() {
  while (!mPreallocatedProcesses.empty()) {
    RefPtr<ContentParent> process(mPreallocatedProcesses.front().forget());
    mPreallocatedProcesses.pop_front();
    process->ShutDownProcess(ContentParent::SEND_SHUTDOWN_MESSAGE);
    // drop ref and let it free
  }

  // Make sure to also clear out the recycled E10S process cache, as it's also
  // controlled by the same preference, and can be cleaned up due to memory
  // pressure.
  if (RefPtr<ContentParent> recycled =
          ContentParent::sRecycledE10SProcess.forget()) {
    recycled->MaybeBeginShutDown();
  }
}

inline PreallocatedProcessManagerImpl*
PreallocatedProcessManager::GetPPMImpl() {
  if (PreallocatedProcessManagerImpl::sShutdown) {
    return nullptr;
  }
  return PreallocatedProcessManagerImpl::Singleton();
}

inline PreallocatedProcessManagerImpl*
PreallocatedProcessManager::GetPPMImplForRecording() {
  if (PreallocatedProcessManagerImpl::sShutdown) {
    return nullptr;
  }
  return PreallocatedProcessManagerImpl::SingletonForRecording();
}

/* static */
bool PreallocatedProcessManager::Enabled() {
  if (auto impl = GetPPMImpl()) {
    return impl->mEnabled;
  }
  return false;
}

/* static */
void PreallocatedProcessManager::AddBlocker(const nsACString& aRemoteType,
                                            ContentParent* aParent) {
  MOZ_LOG(ContentParent::GetLog(), LogLevel::Debug,
          ("AddBlocker: %s %p (sNumBlockers=%d)",
           PromiseFlatCString(aRemoteType).get(), aParent,
           PreallocatedProcessManagerImpl::sNumBlockers));
  PreallocatedProcessManagerImpl::AddBlocker();
}

/* static */
void PreallocatedProcessManager::RemoveBlocker(const nsACString& aRemoteType,
                                               ContentParent* aParent) {
  MOZ_LOG(ContentParent::GetLog(), LogLevel::Debug,
          ("RemoveBlocker: %s %p (sNumBlockers=%d)",
           PromiseFlatCString(aRemoteType).get(), aParent,
           PreallocatedProcessManagerImpl::sNumBlockers));
  PreallocatedProcessManagerImpl::RemoveBlocker();
}

/* static */
already_AddRefed<ContentParent> PreallocatedProcessManager::Take(
    const nsACString& aRemoteType) {
  if (auto impl = GetPPMImpl()) {
    return impl->Take(aRemoteType);
  }
  return nullptr;
}
already_AddRefed<ContentParent> PreallocatedProcessManager::TakeForRecording(
    const nsACString& aRemoteType) {
  if (auto impl = GetPPMImplForRecording()) {
    return impl->Take(aRemoteType);
  }
  return nullptr;
}

/* static */
void PreallocatedProcessManager::Erase(ContentParent* aParent) {
  if (aParent->IsRecording()) {
    if (auto impl = GetPPMImplForRecording()) {
      impl->Erase(aParent);
    }
  } else {
    if (auto impl = GetPPMImpl()) {
      impl->Erase(aParent);
    }
  }
}

}  // namespace mozilla
