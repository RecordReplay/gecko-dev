/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/* eslint-disable spaced-comment, brace-style, indent-legacy, no-shadow */

"use strict";

const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");
const { XPCOMUtils } = ChromeUtils.import(
  "resource://gre/modules/XPCOMUtils.jsm"
);
const { OS } = ChromeUtils.import("resource://gre/modules/osfile.jsm");
const { setTimeout } = Components.utils.import(
  "resource://gre/modules/Timer.jsm"
);

const { EventEmitter } = ChromeUtils.import("resource://gre/modules/EventEmitter.jsm");

const { CryptoUtils } = ChromeUtils.import(
  "resource://services-crypto/utils.js"
);

const ReplayAuth = ChromeUtils.import(
  "resource://devtools/server/actors/replay/auth.js"
);
const { queryAPIServer } = ChromeUtils.import(
  "resource://devtools/server/actors/replay/api-server.js"
);
const { pingTelemetry } = ChromeUtils.import(
  "resource://devtools/server/actors/replay/telemetry.js"
);
const { getenv, setenv } = ChromeUtils.import(
  "resource://devtools/server/actors/replay/env.js"
);

ChromeUtils.defineModuleGetter(
  this,
  "TabStateFlusher",
  "resource:///modules/sessionstore/TabStateFlusher.jsm"
);
ChromeUtils.defineModuleGetter(
  this,
  "SessionStore",
  "resource:///modules/sessionstore/SessionStore.jsm"
);

const {
  getChannelRequestData,
  getChannelResponseData,
  getChannelRequestDoneData,
  getChannelRequestFailedData,
} = ChromeUtils.import(
  "resource://devtools/server/actors/replay/network-helpers.jsm"
);

XPCOMUtils.defineLazyModuleGetters(this, {
  AppUpdater: "resource:///modules/AppUpdater.jsm",
  E10SUtils: "resource://gre/modules/E10SUtils.jsm",
});

XPCOMUtils.defineLazyServiceGetter(
  this,
  "gExternalProtocolService",
  "@mozilla.org/uriloader/external-helper-app-service;1",
  Ci.nsIExternalProtocolService
);

let updateStatusCallback = null;
let connectionStatus = "cloudConnecting.label";
let gShouldValidateUrl = null;

// Return whether all tabs are automatically being recorded.
function isRecordingAllTabs() {
  return getenv("RECORD_ALL_CONTENT")
      || Services.prefs.getBoolPref("devtools.recordreplay.alwaysRecord");
}

// See also GetRecordReplayDispatchServer in ContentParent.cpp
function getDispatchServer() {
  const address = getenv("RECORD_REPLAY_SERVER");
  if (address) {
    return address;
  }
  return Services.prefs.getStringPref("devtools.recordreplay.cloudServer");
}

function openInNewTab(browser, url) {
  const tabbrowser = browser.getTabBrowser();
  const currentTabIndex = tabbrowser.visibleTabs.indexOf(tabbrowser.selectedTab);
  const triggeringPrincipal = Services.scriptSecurityManager.getSystemPrincipal();
  const tab = tabbrowser.addTab(
    url,
    { triggeringPrincipal, index: currentTabIndex === -1 ? undefined : currentTabIndex + 1}
  );
  tabbrowser.selectedTab = tab;
}

function getViewURL(path) {
  let viewHost = "https://app.replay.io";

  // For testing, allow overriding the host for the view page.
  const hostOverride = getenv("RECORD_REPLAY_VIEW_HOST");
  if (hostOverride) {
    viewHost = hostOverride;
  }

  const url = new URL(viewHost);

  if (path) {
    url.pathname = path;
  }

  return url;
}

function setConnectionStatusChangeCallback(callback) {
  updateStatusCallback = callback;
}

function getConnectionStatus() {
  return connectionStatus;
}

const gWorker = new Worker("connection-worker.js");
gWorker.addEventListener("message", ({ data }) => {
  try {
    switch (data.kind) {
      case "stateChange": {
        const { channelId, state } = data;
        gSockets.get(channelId)?._onStateChange(state);
        break;
      }
      case "commandResponse": {
        const { channelId, commandId, result, error } = data;
        gSockets.get(channelId)?._onCommandResponse(commandId, result, error);
        break;
      }
    }
  } catch (e) {
    ChromeUtils.recordReplayLog(`RecordReplaySocketError ${e} ${e.stack}`);
  }
});

let gChannelId = 1;
const gSockets = new Map();

class ProtocolSocket {
  constructor(address) {
    this._channelId = gChannelId++;
    this._state = "connecting";
    this._onStateChangeCallback = null;
    this._commandId = 1;
    this._handlers = new Map();

    gWorker.postMessage({ kind: "openChannel", channelId: this._channelId, address });
    gSockets.set(this._channelId, this);
  }

  get onStateChange() {
    return this._onStateChangeCallback;
  }

  set onStateChange(callback) {
    this._onStateChangeCallback = callback;
    this._notifyStateChange();
  }

  _onStateChange(state) {
    this._state = state;
    this._notifyStateChange();
  }
  _notifyStateChange() {
    this._onStateChangeCallback?.(this._state);
  }

  _onCommandResponse(commandId, result, error) {
    const resolve = this._handlers.get(commandId);
    this._handlers.delete(commandId);
    resolve({ result, error });
  }

  async sendCommand(method, params) {
    const commandId = this._commandId++;
    gWorker.postMessage({ kind: "sendCommand", channelId: this._channelId, commandId, method, params });
    const response = await new Promise(resolve => this._handlers.set(commandId, resolve));

    if (response.error) {
      throw new CommandError(response.error.message, response.error.code, response.error.data);
    }

    return response.result;
  }

  setAccessToken(token) {
    if (!token) {
      throw new Error("Token must be truthy");
    }
    gWorker.postMessage({ kind: "setAccessToken", channelId: this._channelId, token });
  }

  close() {
    gSockets.delete(this._channelId);
    gWorker.postMessage({ kind: "closeChannel", channelId: this._channelId });
  }
}

const gCommandSocket = new ProtocolSocket(getDispatchServer());
gCommandSocket.onStateChange = state => {
  let label;
  switch (state) {
    case "open":
      label = "";
      break;
    case "connecting":
      label = "cloudConnecting.label";
      break;
    case "error":
      label = "cloudError.label";
      break;
    case "close":
      label = "";
      break;
  }

  connectionStatus = label;
  if (updateStatusCallback) {
    updateStatusCallback(connectionStatus);
  }
}

// when the browser restarts itself (usually after an update), the
// environment variables are retained. We want to ensure that this
// variable doesn't contain an outdated token, so we clear it here
// and rely on the initialization code in this file to set it again
// if an API key or a current token is found.
setenv("RECORD_REPLAY_AUTH", null);
let gTokenChangeCallbacks = null;
function setAccessToken(token, isAPIKey) {
  setenv("RECORD_REPLAY_AUTH", token);
  gCommandSocket.setAccessToken(token);

  // If we're working with an API key, there's no way for us to get a new
  // key to try again, so we can bail early and let the retry code figure
  // out what it wants to do.
  if (isAPIKey) {
    if (gTokenChangeCallbacks) {
      throw new Error("Unexpected API key");
    }
    return;
  }
  for (const callback of gTokenChangeCallbacks || []) {
    callback();
  }
  gTokenChangeCallbacks = new Set();
}

const AUTHENTICATION_REQUIRED_CODE = 49;

async function sendCommand(method, params) {
  const tokenCallbacks = gTokenChangeCallbacks;
  try {
    return await gCommandSocket.sendCommand(method, params);
  } catch (err) {
    if (!(err instanceof CommandError) || err.code !== AUTHENTICATION_REQUIRED_CODE) {
      throw err;
    }

    // If there was no set of token callbacks to begin with, we can assume that
    // we'll either have hidden the record button in the first place, or are working
    // with an API key that can't be renewed and thus don't have anything to wait for.
    if (!tokenCallbacks) {
      throw err;
    }

    // If the token hasn't been changed since the first attempt was dispatched,
    // we can let the user know and wait for them to sign in again.
    if (tokenCallbacks === gTokenChangeCallbacks) {
      clearUserToken();
      if (gTokenChangeCallbacks.size === 0) {
        Services.prompt.alert(null, "Replay Authentication", "Your Replay session has expired while recording. \nPlease sign in.");
      }

      await new Promise(resolve => {
        gTokenChangeCallbacks.add(function handler(value) {
          gTokenChangeCallbacks.delete(handler);
          resolve(value);
        });
      });
    }

    return await gCommandSocket.sendCommand(method, params);
  }
}

class CommandError extends Error {
  constructor(message, code, data) {
    super(message);
    this.code = code;
    this.data = data;
  }
}

// Resolve hooks for promises waiting on a recording to be created.
const gRecordingCreateWaiters = [];

function isLoggedIn() {
  const token = ReplayAuth.getReplayUserToken();
  if (token) {
    const expiration = ReplayAuth.tokenExpiration(token);

    return expiration && expiration > Date.now();
  }

  return !!ReplayAuth.hasOriginalApiKey();
}

async function saveRecordingToken(token) {
  ReplayAuth.setReplayUserToken(token);
  gShouldValidateUrl = null;
}

function isRunningTest() {
  return !!getenv("RECORD_REPLAY_TEST_SCRIPT");
}

function clearUserToken() {
  saveRecordingToken(null);
}

// If there is an API key, all authentication in the browser uses that
// key and ignores tokens provided by any logged-in session.
if (ReplayAuth.hasOriginalApiKey()) {
  setAccessToken(ReplayAuth.getOriginalApiKey(), true /* isAPIKey */);
} else {
  let gExpirationTimer;

  const ensureAccessTokenStateSynchronized = function() {
    if (gExpirationTimer) {
      clearTimeout(gExpirationTimer);
      gExpirationTimer = null;
    }

    let token = ReplayAuth.getReplayUserToken();
    if (!token) {
      return;
    }

    const payload = ReplayAuth.tokenInfo(token);
    const expiration = ReplayAuth.tokenExpiration(token);
    if (typeof expiration !== "number") {
      ChromeUtils.recordReplayLog(`InvalidJWTExpiration`);
      clearUserToken();
      return;
    }

    const timeToExpiration = expiration - Date.now();
    if (timeToExpiration <= 0) {
      pingTelemetry("browser", "auth-expired", {
        expiration,
        authId: payload.payload.sub,
      });
      clearUserToken();
      return;
    }

    gExpirationTimer = setTimeout(
      () => {
        pingTelemetry("browser", "auth-expired", {
          expiration,
          authId: payload.payload.sub,
        });
        clearUserToken();
      },
      timeToExpiration
    );

    setAccessToken(token);
  }

  Services.prefs.addObserver("devtools.recordreplay.user-token", () => {
    ensureAccessTokenStateSynchronized();
  });
  ensureAccessTokenStateSynchronized();
}

const SEEN_MANAGERS = new WeakSet();
class Recording extends EventEmitter {
  constructor(pmm) {
    super();
    if (SEEN_MANAGERS.has(pmm)) {
      console.error("Duplicate recording for same child process manager");
    }
    SEEN_MANAGERS.add(pmm);

    this._pmm = pmm;
    this._resourceUploads = [];

    this._recordingResourcesUpload = null;

    this._pmm.addMessageListener("RecordReplayGeneratedSourceWithSourceMap", {
      receiveMessage: msg => this._onNewSourcemap(msg.data),
    });
    this._pmm.addMessageListener("RecordingFinished", {
      receiveMessage: msg => this._onFinished(msg.data),
    });
    this._pmm.addMessageListener("RecordingUnusable", {
      receiveMessage: msg => this._onUnusable(msg.data),
    });
    this._pmm.addMessageListener("RecordingUnsupportedFeature", {
      receiveMessage: msg => this._onUnsupportedFeature(msg.data)
    });
  }

  get osPid() {
    return this._pmm.osPid;
  }

  sendProcessMessage(name, data) {
    this._pmm.sendAsyncMessage(name, data);
  }

  _lockRecording(recordingId) {
    if (this._recordingResourcesUpload) {
      return;
    }

    this._recordingResourcesUpload = sendCommand("Internal.beginRecordingResourceUpload", {
      recordingId: recordingId,
    }).then(
      params => params.key,
      err => {
        console.error("Failed to tell the server about in-progress resource uploading", err);
        pingTelemetry("sourcemap-upload", "lock-exception", {
          message: err?.message,
          stack: err?.stack,
          recordingId,
        });
        // We don't re-throw here because we can at worst let the resources upload
        // might still succeed in the background, it'll just be a race and the sourcemaps
        // may not apply until the user creates a second debugging session.
        return null;
      },
    );
  }

  _unlockRecording(recordingId) {
    if (!this._recordingResourcesUpload) {
      return;
    }

    this._recordingResourcesUpload
      .then(key => {
        if (!key) {
          return;
        }

        return sendCommand("Internal.endRecordingResourceUpload", { key });
      })
      .catch(err => {
        console.error("Exception while unlocking", err);
        pingTelemetry("sourcemap-upload", "unlock-exception", {
          message: err?.message,
          stack: err?.stack,
          recordingId,
        });
      });
  }

  _onNewSourcemap(params) {
    this._lockRecording(params.recordingId);

    this._resourceUploads.push(uploadAllSourcemapAssets(params).catch(err => {
      console.error("Exception while processing sourcemap", err, params);

      pingTelemetry("sourcemap-upload", "upload-exception", {
        message: err?.message,
        stack: err?.stack,
        recordingId: params.recordingId,
        commandErrorData: err instanceof CommandError ? err.data : undefined,
      });
    }));
  }

  async _onFinished(data) {
    const recordingMetadata = {
      id: data.id,
      url: data.url,
      title: data.title,
      duration: data.duration,
    };
    try {
      this.emit("finished", recordingMetadata);

      // Upload the metadata without the screenshot earlier to unblock the
      // upload screen
      await sendCommand("Internal.setRecordingMetadata", {
        authId: undefined,
        recordingData: {...data, lastScreenData: "", lastScreenMimeType: ""},
      });

      this.emit("saved", recordingMetadata);

      // If we locked the recording because of sourcemaps, we should wait
      // that the lock to be initialized before emitting the event so that
      // we don't risk racing lock creation with session creation.
      await this._recordingResourcesUpload;

      await sendCommand("Internal.setRecordingMetadata", {
        authId: undefined,
        recordingData: data,
      });
    } catch (err) {
      console.error("Exception while setting recording metadata", err);
      let message;
      if (err instanceof CommandError) {
        message = ": " + err.message;
      }
      this._onUnusable({ why: "failed to set recording metadata" + message });
      return;
    }

    try {
      // Ensure that all sourcemap resources have been sent to the server before
      // we consider the recording saved, so that we don't risk creating a
      // recording session without all the maps available.
      await Promise.all(this._resourceUploads);
    } finally {
      this._unlockRecording(data.id);
    }
  }

  _onUnusable(data) {
    this._unlockRecording(null);

    this.emit("unusable", data);
  }

  _onUnsupportedFeature(data) {
    this.emit("unsupportedFeature", data);
  }
}

async function checkShouldValidateUrl() {
  if (gShouldValidateUrl === null) {
    const resp = await queryAPIServer(`
      query GetOrgs {
        viewer {
          workspaces {
            edges {
              node {
                isOrganization
                settings {
                  features
                }
              }
            }
          }
        }
      }
    `);

    if (resp.errors) {
      throw new Error("Unexpected error checking Replay user permissions");
    }

    const workspaces = resp.data.viewer?.workspaces.edges;
    gShouldValidateUrl = !workspaces ? false : workspaces.some(w => {
      if (w.node.isOrganization) {
        const {allowList, blockList} = w.node.settings?.features?.recording || {};

        return (Array.isArray(allowList) && allowList.length > 0) || (Array.isArray(blockList) && blockList.length > 0);
      }

      return false;
    });
  }

  return gShouldValidateUrl;
}

async function canRecordUrl(url) {
  try {
    const shouldValidate = await checkShouldValidateUrl();
    if (!shouldValidate) return true;

    const resp = await queryAPIServer(`
      query CanRecord ($url: String!) {
        viewer {
          canRecordUrl(url: $url)
        }
      }
    `, {
      url
    });

    if (resp.errors) {
      throw new Error(resp.errors[0].message);
    }

    return resp.data.viewer.canRecordUrl;
  } catch (e) {
    // Fallback to allowing recordings if the backend errors but log to telemetry
    console.error(e);
    pingTelemetry("recording", "can-record-failed", { why: e.message || "", url });

    return true;
  }
}

function getLocationListener(key) {
  return {
    key,
    QueryInterface: ChromeUtils.generateQI([
      "nsIWebProgressListener",
      "nsISupportsWeakReference",
    ]),
    onLocationChange(aWebProgress, _, aLocation) {
      if(!aWebProgress.isTopLevel) return;

      // Always allow blank and new tab
      if (aLocation.displaySpec === "about:blank" || aLocation.displaySpec === "https://app.replay.io/browser/new-tab") {
        return;
      }

      canRecordUrl(aLocation.displaySpec).then((canRecord) => {
        if (canRecord) return;

        const browser = getRecordingBrowser(this.key);
        const message = `The URL ${aLocation.displaySpec} may not be recorded according to your organization's policy.`;
        showInvalidatedRecordingNotification(browser, message);
        const remoteTab = browser.frameLoader.remoteTab;
        if (remoteTab) {
          remoteTab.finishRecording("Organization Policy Violation");
        }
      });
    }
  }
}

Services.obs.addObserver(
  subject => {
    const {state, entry, browser} = subject.wrappedJSObject;
    if (browser && entry) {
      if (state === RecordingState.STARTING) {
        browser.addProgressListener(entry.locationListener);
      } else if (state === RecordingState.READY) {
        browser.removeProgressListener(entry.locationListener)
      }
    }
  },
  "recordreplay-recording-changed"
);

const RecordingState = {
  READY: 0,
  STARTING: 1,
  RECORDING: 2,
  STOPPING: 3
};

const recordings = new Map();

function getRecordingKey(browser) {
  return browser.frameLoader;
}

function getRecordingBrowser(key) {
  return key.ownerElement;
}

function updateRecordingState(key, state) {
  const current = recordings.get(key);
  const locationListener = current && current.locationListener || getLocationListener(key);
  const timestamps = current && current.timestamps || {};
  timestamps[state] = Date.now();

  const entry = {
    state,
    recording: current ? current.recording : null,
    timestamps,
    locationListener,
  };
  recordings.set(key, entry);
  return entry;
}

function addRecordingInstance(key, recording) {
  const entry = recordings.get(key);
  if (!entry) {
    entry = updateRecordingState(key, RecordingState.RECORDING);
  }
  entry.recording = recording;
}

function setRecordingState(key, state) {
  let entry = null;
  if (state === RecordingState.READY) {
    entry = recordings.get(key);
    recordings.delete(key);
  } else {
    entry = updateRecordingState(key, state);
  }

  Services.obs.notifyObservers({
    browser: getRecordingBrowser(key),
    entry,
    state
  }, "recordreplay-recording-changed");
}

function getRecordingState(browser) {
  const {state} = recordings.get(getRecordingKey(browser)) || {state: RecordingState.READY};

  return state;
}

// Returns the time, in ms, since the browser entered `state`. If the duration
// can't be calculated because a prior timestamp value isn't available for the
// given browser, returns -1.
function getRecordingStateDuration(key, state) {
  const now = Date.now();
  const stateObj = recordings.get(key);
  const ts = stateObj && stateObj.timestamps[state];

  if (!ts) return -1;

  return now - ts;
}

// If an action invalidates the key (like updateBrowserRemoteness), we need to
// remap the state from the old key to the new key.
function remapRecordingState(browser, key) {
  const newKey = getRecordingKey(browser);

  if (recordings.has(key)) {
    const entry = recordings.get(key);
    // Remap the key used by the listener so it can find the browser
    if (entry.locationListener) {
      entry.locationListener.key = newKey;
    }
    recordings.delete(key);
    recordings.set(newKey, entry);
  }

  return newKey;
}

function isRecording(browser) {
  const {state} = getRecordingState(getRecordingKey(browser));

  return state === RecordingState.RECORDING || browser.hasAttribute(
    "recordExecution"
  ) || isRecordingAllTabs();
}

function toggleRecording(browser) {
  const key = getRecordingKey(browser);

  let state = RecordingState.READY;
  if (recordings.has(key)) {
    state = recordings.get(key).state;
  } else {
    updateRecordingState(key, RecordingState.READY);
  }

  // Some sort of delay seems required to allow the chrome to update the
  // button to show the spinner. It might be possible to lower the timeout
  // but < 50ms was never enough but 100ms seems to be always enough.
  if (state === RecordingState.READY) {
    pingTelemetry("recording", "start", { action: "click", recordingState: state });
    setRecordingState(key, RecordingState.STARTING);
    setTimeout(() => startRecording(browser), 100);
  } else if (state === RecordingState.RECORDING) {
    pingTelemetry("recording", "stop", { action: "click", recordingState: state });
    setRecordingState(key, RecordingState.STOPPING);
    setTimeout(() => stopRecording(browser), 100);
  }
}

async function startRecording(browser) {
  let key = getRecordingKey(browser);
  const {state} = recordings.get(key) || {};

  if (!browser || state !== RecordingState.STARTING) {
    setRecordingState(key, RecordingState.READY);
    pingTelemetry("recording", "start-failed", { why: browser ? "invalid recording state" : "browser undefined", recordingState: state });
    return;
  }

  const tabbrowser = browser.getTabBrowser();
  const tab = tabbrowser.selectedTab;

  let url = browser.currentURI.spec;

  // Don't preprocess recordings if we will be submitting them for testing.
  try {
    if (
      Services.prefs.getBoolPref("devtools.recordreplay.submitTestRecordings")
    ) {
      setenv("RECORD_REPLAY_DONT_PROCESS_RECORDINGS", "1");
    }
  } catch (e) {}

  // The recording process uses this env var when printing out the recording ID.
  setenv("RECORD_REPLAY_URL", url);

  let remoteType = E10SUtils.getRemoteTypeForURI(
    url,
    /* aMultiProcess */ true,
    /* aRemoteSubframes */ false,
    /* aPreferredRemoteType */ undefined,
    /* aCurrentUri */ null
  );
  if (
    remoteType != E10SUtils.WEB_REMOTE_TYPE &&
    remoteType != E10SUtils.FILE_REMOTE_TYPE
  ) {
    url = "about:blank";
    remoteType = E10SUtils.WEB_REMOTE_TYPE;
  }

  // Before reading the tab state, we need to be sure that the parent process
  // has full session state. The user (or more likely automated tests), could
  // easily have begun recording while the initial page was still loading,
  // in which case the parent may not have initialized the session fully yet.
  await TabStateFlusher.flush(browser);

  pingTelemetry("recording", "start", { action: "updateBrowserRemoteness", recordingState: state, duration: getRecordingStateDuration(key, RecordingState.STARTING)});
  const tabState = SessionStore.getTabState(tab);
  tabbrowser.updateBrowserRemoteness(browser, {
    recordExecution: getDispatchServer(url),
    newFrameloader: true,
    remoteType,
  });

  // Creating a new frameloader will destroy the tab's session history so we
  // need to restore it. This also instructs the new child proocess to load
  // the target URL, which would otherwise require a browser.loadURI() call.
  SessionStore.setTabState(tab, tabState);

  key = remapRecordingState(browser, key);
  setRecordingState(key, RecordingState.RECORDING);
  pingTelemetry("recording", "start", { action: "complete", recordingState: state, duration: getRecordingStateDuration(key, RecordingState.STARTING) });
}

function stopRecording(browser) {
  const key = getRecordingKey(browser);
  const {state} = recordings.get(key) || {};

  if (!browser || state !== RecordingState.STOPPING)  {
    pingTelemetry("recording", "stop-failed", { why: browser ? "invalid recording state" : "browser undefined", recordingState: state, duration: getRecordingStateDuration(key, RecordingState.STOPPING) });
    setRecordingState(key, RecordingState.READY);
    return;
  }

  const remoteTab = browser.frameLoader.remoteTab;
  if (!remoteTab || !remoteTab.finishRecording()) {
    pingTelemetry("recording", "stop-failed", { why: remoteTab ? "finishRecording failed" : "remoteTab undefined", recordingState: state, duration: getRecordingStateDuration(key, RecordingState.STOPPING) });
    setRecordingState(key, RecordingState.READY);
    return;
  }

  pingTelemetry("recording", "stop", { action: "complete", duration: getRecordingStateDuration(key, RecordingState.STOPPING) });
  ChromeUtils.recordReplayLog(`WaitForFinishedRecording`);
}

function setRecordingFinished(browser, url) {
  const key = getRecordingKey(browser);
  const recordingState = getRecordingState(browser);

  if (isRecordingAllTabs()) {
    return;
  }

  const tabbrowser = browser.getTabBrowser();
  const tab = tabbrowser.getTabForBrowser(browser);
  const contentPrincipal = browser.contentPrincipal;

  pingTelemetry("recording", "finished", { action: "updateBrowserRemoteness", recordingState });
  const state = SessionStore.getTabState(tab);
  tabbrowser.updateBrowserRemoteness(browser, {
    recordExecution: undefined,
    newFrameloader: true,
    remoteType: E10SUtils.WEB_REMOTE_TYPE,
  });

  if (!url) {
    const contentUrl = browser.currentURI.spec;

    pingTelemetry("recording", "finished", { action: "reloadContentUrl", recordingState });
    browser.loadURI(contentUrl, { triggeringPrincipal: contentPrincipal });
  }

  // Creating a new frameloader will destroy the tab's session history so we
  // need to restore it.
  SessionStore.setTabState(tab, state);

  if (url) {
    openInNewTab(browser, url);
  }

  remapRecordingState(browser, key);
  pingTelemetry("recording", "finished", { action: "complete", recordingState });
}

function setRecordingSaved(browser, recordingId) {
  // suppress launching new tab for test recordings
  if (Services.prefs.getBoolPref("devtools.recordreplay.submitTestRecordings")) {
    return;
  }

  // Find the dispatcher to connect to.
  const dispatchAddress = getDispatchServer();
  const key = getRecordingKey(browser);

  const url = getViewURL(`/recording/${recordingId}`);

  // Specify the dispatch address if it is not the default.
  if (dispatchAddress != "wss://dispatch.replay.io") {
    url.searchParams.set('dispatch', dispatchAddress);
  }

  // For testing, allow specifying a test script to load in the tab.
  const localTest = getenv("RECORD_REPLAY_LOCAL_TEST");
  if (localTest) {
    url.searchParams.set('test', localTest);
  }

  openInNewTab(browser, url.toString());

  // defer setting the state until the end so that the new tab has opened before the spinner disappears.
  setRecordingState(key, RecordingState.READY, {duration: getRecordingStateDuration(key, RecordingState.STOPPING)});
}

function handleRecordingStarted(pmm) {
  const recording = new Recording(pmm);

  // There can occasionally be times when the browser isn't found when the
  // recording begins, so we lazily look it up the first time it is needed.
  let _browser = null;
  function getBrowser() {
    for (let frameLoader of recordings.keys()) {
      if (frameLoader.remoteTab && frameLoader.remoteTab.osPid === pmm.osPid) {
        _browser = frameLoader.ownerElement;
        break;
      }
    }
    return _browser;
  }

  addRecordingInstance(getRecordingKey(getBrowser()), recording);

  recording.on("unusable", function(name, data) {
    pingTelemetry("recording", "unusable", data);

    // Log the reason so we can see in our CI logs when something went wrong.
    console.error("Unstable recording: " + data.why);
    const browser = getBrowser();

    // Sometimes, an unusable recording causes the browser to be cleaned
    // up before this point.  Check for this and emit a clear telemetry
    // event instead of an internal crash (getRecordingKey failing).
    if (!browser) {
      pingTelemetry("recording", "unusable-browser-died", data);
      // Log the reason so we can see in our CI logs when something went wrong.
      console.error("Browser was destroyed before 'unusable' handler ran.");
      return;
    }

    hideUnsupportedFeatureNotification(browser);

    const url = getViewURL('/browser/error');
    url.searchParams.set("message", data.why);
    setRecordingFinished(browser, url.toString());
    setRecordingState(getRecordingKey(browser), RecordingState.READY);
  });

  recording.on("finished", function(name, data) {
    const recordingId = data.id;

    pingTelemetry("recording", "finished", {...data, recordingId});

    try {
      const browser = getBrowser();
      let url;

      hideUnsupportedFeatureNotification(browser);

      // When the submitTestRecordings pref is set we don't load the viewer,
      // but show a simple page that the recording was submitted, to make things
      // simpler for QA and provide feedback that the pref was set correctly.
      if (
        Services.prefs.getBoolPref("devtools.recordreplay.submitTestRecordings")
      ) {
        fetch(`https://test-inbox.replay.io/${recordingId}:${browser.currentURI.spec}`);
        const why = `Test recording added: ${recordingId}`;
        url = `about:replay?submitted=${why}`;
      }

      setRecordingFinished(browser, url);
    } catch (e) {
      pingTelemetry("recording", "finished-error", {...data, recordingId, error: e});
    }

    ChromeUtils.recordReplayLog(`FinishedRecording ${recordingId}`);
  });

  recording.on("saved", function(name, data) {
    const recordingId = data.id;

    pingTelemetry("recording", "saved", {...data, recordingId});

    try {
      const browser = getBrowser();
      setRecordingSaved(browser, recordingId);
    } catch (e) {
      pingTelemetry("recording", "save-error", {...data, recordingId, error: e});
    }

    ChromeUtils.recordReplayLog(`SavedRecording ${recordingId}`);
  });

  recording.on("unsupportedFeature", function(name, data) {
    const browser = getBrowser();
    showUnsupportedFeatureNotification(browser, data.feature, data.issueNumber);
  });
}

function showInvalidatedRecordingNotification(browser, message = `The current recording is not allowed by your organization's policy.`) {
  const notificationBox = browser.getTabBrowser().getNotificationBox(browser);
  let notification = notificationBox.getNotificationWithValue(
    "replay-invalidated-recording"
  );
  if (notification) {
    return;
  }

  notificationBox.appendNotification(
    message,
    "replay-invalidated-recording",
    undefined,
    notificationBox.PRIORITY_WARNING_HIGH,
  );
}

function hideInvalidatedRecordingNotification(browser) {
  const notificationBox = browser.getTabBrowser().getNotificationBox(browser);
  const notification = notificationBox.getNotificationWithValue(
    "replay-invalidated-recording"
  );

  if (notification) {
    notificationBox.removeNotification(notification)
  }
}

function showUnsupportedFeatureNotification(browser, feature, issueNumber) {
  const notificationBox = browser.getTabBrowser().getNotificationBox(browser);
  let notification = notificationBox.getNotificationWithValue(
    "replay-unsupported-feature"
  );
  if (notification) {
    return;
  }

  const message = `${feature} is not currently supported.`;

  notificationBox.appendNotification(
    message,
    "replay-unsupported-feature",
    undefined,
    notificationBox.PRIORITY_WARNING_HIGH,
    [{
      label: "Learn More",
      callback: () => {
        openInNewTab(browser, `https://github.com/recordreplay/gecko-dev/issues/${issueNumber}`);
      }
    }],
  );
}

function hideUnsupportedFeatureNotification(browser) {
  const notificationBox = browser.getTabBrowser().getNotificationBox(browser);
  const notification = notificationBox.getNotificationWithValue(
    "replay-unsupported-feature"
  );

  if (notification) {
    notificationBox.removeNotification(notification)
  }
}

function uploadSourceMap(
  recordingId,
  mapText,
  baseURL,
  { targetContentHash, targetURLHash, targetMapURLHash }
) {
  return withUploadedResource(mapText, async (resource) => {
    const result = await sendCommand("Recording.addSourceMap", {
      recordingId,
      resource,
      baseURL,
      targetContentHash,
      targetURLHash,
      targetMapURLHash,
    })
    return result.id;
  });
}

async function uploadAllSourcemapAssets({
  recordingId,
  targetURLHash,
  targetContentHash,
  targetMapURLHash,
  sourceMapURL,
  sourceMapBaseURL
}) {
  const result = await fetchText(recordingId, sourceMapURL);
  if (!result) {
    return;
  }
  const mapText = result.text;

  const { sources } =
    collectUnresolvedSourceMapResources(mapText, sourceMapURL, sourceMapBaseURL);

  let mapUploadFailed = false;
  let mapIdPromise;
  function ensureMapUploading() {
    if (!mapIdPromise) {
      mapIdPromise = uploadSourceMap(recordingId, mapText, sourceMapBaseURL, {
        targetContentHash,
        targetURLHash,
        targetMapURLHash
      });
      mapIdPromise.catch(() => {
        mapUploadFailed = true;
      });
    }
    return mapIdPromise;
  }

  await Promise.all([
    // For data URLs, we don't want to start uploading the map by default
    // because for most data: URLs, the inline sources will contain
    // everything needed for debugging, and the server can resolve
    // data: URLs itself without needing resources to be uploaded.
    // If the data: map _does_ need to be uploaded, that will be handled
    // once that is detected by the sources.
    sourceMapURL.startsWith("data:") ? undefined : ensureMapUploading(),
    Promise.all(sources.map(async ({ offset, url }) => {
      const result = await fetchText(recordingId, url);
      if (!result || mapUploadFailed) {
        return;
      }

      await Promise.all([
        // Once we know there are original sources that we can upload, we want
        // ensure that the map is uploading, if it wasn't already.
        ensureMapUploading(),
        withUploadedResource(result.text, async (resource) => {
          let parentId;
          try {
            parentId = await ensureMapUploading();
          } catch (err) {
            // The error will be handled above, but if it fails,
            // that we don't bother seeing the failure that should
            // trigger a retry of this.
            return;
          }

          await sendCommand("Recording.addOriginalSource", {
            recordingId,
            resource,
            parentId,
            parentOffset: offset,
          });
        })
      ]);
    })),
  ]);
}

function collectUnresolvedSourceMapResources(mapText, mapURL, mapBaseURL) {
  let obj;
  try {
    obj = JSON.parse(mapText);
    if (typeof obj !== "object" || !obj) {
      return {
        sources: [],
      };
    }
  } catch (err) {
    console.error("Exception parsing sourcemap JSON", mapURL);
    return {
      sources: [],
    };
  }

  function logError(msg) {
    console.error(msg, mapURL, sourceOffset);
  }

  const unresolvedSources = [];
  let sourceOffset = 0;

  if (obj.version !== 3) {
    logError("Invalid sourcemap version");
    return {
      sources: [],
    };
  }

  if (obj.sources != null) {
    const { sourceRoot, sources, sourcesContent } = obj;

    if (Array.isArray(sources)) {
      for (let i = 0; i < sources.length; i++) {
        const offset = sourceOffset++;

        if (
          !Array.isArray(sourcesContent) ||
          typeof sourcesContent[i] !== "string"
        ) {
          let url = sources[i];
          if (typeof sourceRoot === "string" && sourceRoot) {
            url = sourceRoot.replace(/\/?/, "/") + url;
          }
          let sourceURL;
          try {
            sourceURL = new URL(url, mapBaseURL).toString();
          } catch {
            logError("Unable to compute original source URL: " + url);
            continue;
          }

          unresolvedSources.push({
            offset,
            url: sourceURL,
          });
        }
      }
    } else {
      logError("Invalid sourcemap source list");
    }
  }

  return {
    sources: unresolvedSources,
  };
}

async function fetchText(recordingId, url) {
  let urlObj;
  try {
    urlObj = new URL(url);
  } catch (err) {
    // If the URL isn't parseable then the fetch is definitely going to fail
    // so we might as well just print a warning.
    urlObj = null;
  }
  // For URLs like webpack:///foo/bar.js, we can just warn and then
  // ignore it since it'd never load anyway.
  if (urlObj && !["http:", "https:", "data:", "blob:"].includes(urlObj.protocol)) {
    urlObj = null;
  }
  if (!urlObj) {
    console.warn("Unable to fetch recording resource", url);
    return null;
  }

  try {
    const response = await fetch(url, {
      //
      credentials: "include",
    });
    if (response.status < 200 || response.status >= 300) {
      console.error("Error fetching recording resource", url, response);
      pingTelemetry("sourcemap-upload", "fetch-bad-status", {
        message: `Request got status: ${response.status}`,
        status: response.status,
        url: ["http:", "https:"].includes(urlObj.protocol) ? url : urlObj.protocol,
        recordingId,
      });
      return null;
    }

    return {
      url,
      text: await response.text(),
    };
  } catch (e) {
    console.error("Exception fetching recording resource", url, e);
    pingTelemetry("sourcemap-upload", "fetch-exception", {
      message: e?.message,
      stack: e?.stack,
      url: ["http:", "https:"].includes(urlObj.protocol) ? url : urlObj.protocol,
      recordingId,
    });
    return null;
  }
}

async function uploadResource(text) {
  const hash = "sha256:" + CryptoUtils.sha256(text);
  const { token } = await sendCommand("Resource.token", { hash });
  let resource = {
    token,
    saltedHash: "sha256:" + CryptoUtils.sha256(token + text)
  };

  const { exists } = await sendCommand("Resource.exists", { resource });
  if (!exists) {
    ({ resource } = await sendCommand("Resource.create", { content: text }));
  }
  return resource;
}

const RETRY_COUNT = 3;

async function withUploadedResource(text, callback) {
  for (let i = 0; i < RETRY_COUNT - 1; i++) {
    try {
      return await callback(await uploadResource(text));
    } catch (err) {
      // If the connection dies, we want to retry, and if it died and
      // reconnected while something else was going on, the token will
      // likely have been invalidated, so we want to retry in that case too.
      if (err instanceof CommandError && (err.code === -1 || err.code === 39) ) {
        console.error("Resource Upload failed, retrying", err);
        continue;
      }
      throw err;
    }
  }

  return callback(await uploadResource(text));
}

Services.ppmm.addMessageListener("RecordingStarting", {
  receiveMessage(msg) {
    handleRecordingStarted(msg.target);
  },
});

///////////////////////////////////////////////////////////////////////////////
// Network Observing Logic
//
// See module.js for more information about the architecture of Replay's
// network-request observation logic.
///////////////////////////////////////////////////////////////////////////////

function getChannelRecording(channel) {
  for (const [frameLoader, entry] of recordings) {
    if (frameLoader.browsingContext && frameLoader.browsingContext === channel.loadInfo.browsingContext) {
      return entry.recording || null;
    }
  }

  return null;
}
function ensureHttpChannel(channel) {
  if (
    !(channel instanceof Ci.nsIHttpChannel) ||
    !(channel instanceof Ci.nsIClassifiedChannel)
  ) {
    return null;
  }

  channel = channel.QueryInterface(Ci.nsIHttpChannel);
  channel = channel.QueryInterface(Ci.nsIClassifiedChannel);

  if (channel instanceof Ci.nsIHttpChannelInternal) {
    channel = channel.QueryInterface(Ci.nsIHttpChannelInternal);
  }

  return channel;
}

Services.obs.addObserver((subject, topic, data) => {
  const channel = ensureHttpChannel(subject);
  const recording = channel ? getChannelRecording(channel) : null;
  if (!recording) {
    return;
  }

  sendChannelRequestStart(recording, channel);
}, "http-on-opening-request");

Services.obs.addObserver((subject, topic, data) => {
  const channel = ensureHttpChannel(subject);
  const recording = channel ? getChannelRecording(channel) : null;
  if (!recording) {
    return;
  }

  sendChannelRequestStart(recording, channel);
  recording.sendProcessMessage("RecordingChannelRequestFailed", {
    channelId: channel.channelId,
    data: getChannelRequestFailedData(channel),
  });
}, "http-on-failed-opening-request");

function sendChannelRequestStart(recording, channel) {
  // Some requests like the top-level HTTP request appear to be started from the
  // parent process so we proxy those through to the child here since it won't
  // have gotten an http-on-opening-request topic notification.
  recording.sendProcessMessage("RecordingChannelOpeningRequest", {
    channelId: channel.channelId,
    data: getChannelRequestData(channel),
  });
}

Services.obs.addObserver((subject, topic, data) => {
  const channel = ensureHttpChannel(subject);
  if (!channel) {
    return;
  }

  sendChannelResponseStart(channel, true);
}, "http-on-examine-cached-response");

Services.obs.addObserver((subject, topic, data) => {
  const channel = ensureHttpChannel(subject);
  if (!channel) {
    return;
  }

  sendChannelResponseStart(channel, false);
}, "http-on-examine-response");

function sendChannelResponseStart(channel, fromCache) {
  const recording = getChannelRecording(channel);
  if (!recording) {
    return;
  }

  // If we're reading from cache, there may not have been an http-on-opening-request
  // notification for this channel.
  if (fromCache) {
    sendChannelRequestStart(recording, channel);
  }

  recording.sendProcessMessage("RecordingChannelResponseStart", {
    channelId: channel.channelId,
    data: getChannelResponseData(channel, fromCache),
  });
}

const distributor =
  Cc["@mozilla.org/network/http-activity-distributor;1"]
    .getService(Ci.nsIHttpActivityDistributor);
distributor.addObserver({
  observeActivity(
    channel,
    activityType,
    activitySubtype,
    timestamp,
    extraSizeData,
    extraStringData
  ) {
    channel = ensureHttpChannel(channel);
    const recording = channel ? getChannelRecording(channel) : null;
    if (!recording) {
      return;
    }

    if (activityType === Ci.nsIHttpActivityObserver.ACTIVITY_TYPE_HTTP_TRANSACTION) {
      if (activitySubtype === Ci.nsIHttpActivityObserver.ACTIVITY_SUBTYPE_REQUEST_HEADER) {
        recording.sendProcessMessage("RecordingChannelRequestRawHeaders", {
          channelId: channel.channelId,
          requestRawHeaders: extraStringData,
        });
      } else if (activitySubtype === Ci.nsIHttpActivityObserver.ACTIVITY_SUBTYPE_RESPONSE_HEADER) {
        recording.sendProcessMessage("RecordingChannelResponseRawHeaders", {
          channelId: channel.channelId,
          responseRawHeaders: extraStringData,
        });
      }
    }
  },
});

function openSigninPage() {
  const key = btoa(Array.from({length: 32}, () => String.fromCodePoint(Math.floor(Math.random() * 256))).join(""))
  const url = Services.io.newURI(`http://localhost:8080/api/browser/auth?key=${key}`);

  gExternalProtocolService
    .getProtocolHandlerInfo("https")
    .launchWithURI(url);

  Promise.race([
    new Promise((_resolve, reject) => setTimeout(reject, 2 * 60 * 1000)),
    new Promise(async (resolve, reject) => {
      let retries = 0;
      while (retries < 20) {
        const resp = await queryAPIServer(`
          mutation CloseAuthRequest($key: String!) {
            closeAuthRequest(input: {key: $key}) {
              success
              token
            }
          }
        `, {
          key
        });

        if (resp.errors) {
          retries++;
          await new Promise(resolve => setTimeout(resolve, 1000));
        } else {
          ReplayAuth.setReplayUserToken(resp.data.closeAuthRequest.token);
          resolve();
          break;
        }
      }

      reject(`Failed to authenticate`);
    })
  ]).catch(console.error);
}

// eslint-disable-next-line no-unused-vars
var EXPORTED_SYMBOLS = [
  "setConnectionStatusChangeCallback",
  "getConnectionStatus",
  "getDispatchServer",
  "isRecordingAllTabs",
  "isRecording",
  "toggleRecording",
  "getRecordingState",
  "RecordingState",
  "isLoggedIn",
  "saveRecordingToken",
  "isRunningTest",
  "openSigninPage"
];
