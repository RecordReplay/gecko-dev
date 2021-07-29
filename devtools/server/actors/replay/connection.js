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

const { pingTelemetry } = ChromeUtils.import(
  "resource://devtools/server/actors/replay/telemetry.js"
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

XPCOMUtils.defineLazyModuleGetters(this, {
  AppUpdater: "resource:///modules/AppUpdater.jsm",
  E10SUtils: "resource://gre/modules/E10SUtils.jsm",
});

let updateStatusCallback = null;
let connectionStatus = "cloudConnecting.label";

function getenv(name) {
  const env = Cc["@mozilla.org/process/environment;1"].getService(
    Ci.nsIEnvironment
  );
  return env.get(name);
}

function setenv(name, value) {
  const env = Cc["@mozilla.org/process/environment;1"].getService(
    Ci.nsIEnvironment
  );
  return env.set(name, value);
}

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

function getViewURL() {
  let viewHost = "https://replay.io";

  // For testing, allow overriding the host for the view page.
  const hostOverride = getenv("RECORD_REPLAY_VIEW_HOST");
  if (hostOverride) {
    viewHost = hostOverride;
  }
  return `${viewHost}/view`;
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
      throw new CommandError(response.error.message, response.error.code);
    }

    return response.result;
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

async function sendCommand(method, params) {
  return gCommandSocket.sendCommand(method, params);
}

class CommandError extends Error {
  constructor(message, code) {
    super(message);
    this.code = code;
  }
}

// Resolve hooks for promises waiting on a recording to be created.
const gRecordingCreateWaiters = [];

function isAuthenticationEnabled() {
  // Authentication is controlled by a preference but can be disabled by an
  // environment variable.
  return (
    Services.prefs.getBoolPref(
      "devtools.recordreplay.authentication-enabled"
    ) && !getenv("RECORD_REPLAY_DISABLE_AUTHENTICATION")
  );
}

function isRunningTest() {
  return !!getenv("RECORD_REPLAY_TEST_SCRIPT");
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
  }

  get osPid() {
    return this._pmm.osPid;
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
        // We don't re-throw here because we can at worst let the resources upload
        // might still succeed in the background, it'll just be a race and the sourcemaps
        // may not apply until the user creates a second debugging session.
        return null;
      },
    );
  }

  _unlockRecording() {
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
      });
  }

  _onNewSourcemap(params) {
    this._lockRecording(params.recordingId);

    this._resourceUploads.push(uploadAllSourcemapAssets(params).catch(err => {
      console.error("Exception while processing sourcemap", err, params);
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
      const authId = getLoggedInUserAuthId();

      this.emit("finished", recordingMetadata);

      // Upload the metadata without the screenshot earlier to unblock the
      // upload screen
      await sendCommand("Internal.setRecordingMetadata", {
        authId,
        recordingData: {...data, lastScreenData: "", lastScreenMimeType: ""},
      });

      this.emit("saved", recordingMetadata);

      // If we locked the recording because of sourcemaps, we should wait
      // that the lock to be initialized before emitting the event so that
      // we don't risk racing lock creation with session creation.
      await this._recordingResourcesUpload;

      await sendCommand("Internal.setRecordingMetadata", {
        authId,
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
      this._unlockRecording();
    }
  }

  _onUnusable(data) {
    this._unlockRecording();

    this.emit("unusable", data);
  }
}

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

function setRecordingState(key, state) {
  recordings.set(key, {
    state
  });

  Services.obs.notifyObservers({
    browser: getRecordingBrowser(key),
    state
  }, "recordreplay-recording-changed");
}

function getRecordingState(browser) {
  const {state} = recordings.get(getRecordingKey(browser)) || {state: RecordingState.READY};

  return state;
}

// If an action invalidates the key (like updateBrowserRemoteness), we need to
// remap the state from the old key to the new key.
function remapRecordingState(browser, key) {
  const newKey = getRecordingKey(browser);

  if (recordings.has(key)) {
    const entry = recordings.get(key);
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
    recordings.set(key, {
      state: RecordingState.READY
    });
  }

  // Some sort of delay seems required to allow the chrome to update the
  // button to show the spinner. It might be possible to lower the timeout
  // but < 50ms was never enough but 100ms seems to be always enough.
  if (state === RecordingState.READY) {
    pingTelemetry('recording', 'start');
    setRecordingState(key, RecordingState.STARTING);
    setTimeout(() => startRecording(browser), 100);
  } else if (state === RecordingState.RECORDING) {
    pingTelemetry('recording', 'stop');
    setRecordingState(key, RecordingState.STOPPING);
    setTimeout(() => stopRecording(browser), 100);
  }
}

async function startRecording(browser) {
  let key = getRecordingKey(browser);
  const {state} = recordings.get(key) || {};

  if (!browser || state !== RecordingState.STARTING) {
    setRecordingState(key, RecordingState.READY);
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
  // easily have begin recording while the initial page was still loading,
  // in which case the parent may not have initialized the session fully yet.
  await TabStateFlusher.flush(browser);

  const tabState = SessionStore.getTabState(tab);
  tabbrowser.updateBrowserRemoteness(browser, {
    recordExecution: getDispatchServer(url),
    newFrameloader: true,
    remoteType,
  });

  browser.loadURI(url, {
    triggeringPrincipal: browser.contentPrincipal,
  });

  // Creating a new frameloader will destroy the tab's session history so we
  // need to restore it, and we need to do this _after_ `loadURI` so that
  // it doesn't add a new entry to the history.
  SessionStore.setTabState(tab, tabState);

  key = remapRecordingState(browser, key);
  setRecordingState(key, RecordingState.RECORDING);
}

function stopRecording(browser) {
  const key = getRecordingKey(browser);
  const {state} = recordings.get(key) || {};

  if (!browser || state !== RecordingState.STOPPING)  {
    setRecordingState(key, RecordingState.READY);
    return;
  }

  const remoteTab = browser.frameLoader.remoteTab;
  if (!remoteTab || !remoteTab.finishRecording()) {
    setRecordingState(key, RecordingState.READY);
    return;
  }

  ChromeUtils.recordReplayLog(`WaitForFinishedRecording`);
}

function setRecordingFinished(browser, url) {
  const key = getRecordingKey(browser);

  if (isRecordingAllTabs()) {
    return;
  }

  const tabbrowser = browser.getTabBrowser();
  const tab = tabbrowser.getTabForBrowser(browser);
  const contentPrincipal = browser.contentPrincipal;

  const state = SessionStore.getTabState(tab);
  tabbrowser.updateBrowserRemoteness(browser, {
    recordExecution: undefined,
    newFrameloader: true,
    remoteType: E10SUtils.WEB_REMOTE_TYPE,
  });

  if (!url) {
    const contentUrl = browser.currentURI.spec;

    browser.loadURI(contentUrl, { triggeringPrincipal: contentPrincipal });
  }

  // Creating a new frameloader will destroy the tab's session history so we
  // need to restore it.
  SessionStore.setTabState(tab, state);

  if (url) {
    browser.loadURI(url, {
      triggeringPrincipal: Services.scriptSecurityManager.getSystemPrincipal()
    });
  }

  remapRecordingState(browser, key);
}

function setRecordingSaved(browser, recordingId) {
  // suppress launching new tab for test recordings
  if (Services.prefs.getBoolPref("devtools.recordreplay.submitTestRecordings")) {
    return;
  }

  // Find the dispatcher to connect to.
  const dispatchAddress = getDispatchServer();
  const key = getRecordingKey(browser);

  let extra = "";

  // Specify the dispatch address if it is not the default.
  if (dispatchAddress != "wss://dispatch.replay.io") {
    extra += `&dispatch=${dispatchAddress}`;
  }

  // For testing, allow specifying a test script to load in the tab.
  const localTest = getenv("RECORD_REPLAY_LOCAL_TEST");
  if (localTest) {
    extra += `&test=${localTest}`;
  } else if (!isAuthenticationEnabled()) {
    // Adding this urlparam disables checks in the devtools that the user has
    // permission to view the recording.
    extra += `&test=1`;
  }

  const tabbrowser = browser.getTabBrowser();
  const currentTabIndex = tabbrowser.visibleTabs.indexOf(tabbrowser.selectedTab);
  const triggeringPrincipal = Services.scriptSecurityManager.getSystemPrincipal();
  const tab = tabbrowser.addTab(
    `${getViewURL()}?id=${recordingId}${extra}`,
    { triggeringPrincipal, index: currentTabIndex === -1 ? undefined : currentTabIndex + 1}
  );
  tabbrowser.selectedTab = tab;

  // defer setting the state until the end so that the new tab has opened before the spinner disappears.
  setRecordingState(key, RecordingState.READY);
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

  recording.on("unusable", function(name, data) {
    pingTelemetry('recording', 'unusable', data);

    // Log the reason so we can see in our CI logs when something went wrong.
    console.error("Unstable recording: " + data.why);
    const browser = getBrowser();
    const key = getRecordingKey(browser);

    setRecordingFinished(browser, `https://replay.io/browser/error?message=${data.why}`);
    setRecordingState(key, RecordingState.READY);
  });

  recording.on("finished", function(name, data) {
    const recordingId = data.id;

    pingTelemetry('recording', 'finished', {...data, recordingId});

    try {
      const browser = getBrowser();
      let url;

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
      pingTelemetry('recording', 'finished-error', {...data, recordingId, error: e});
    }

    ChromeUtils.recordReplayLog(`FinishedRecording ${recordingId}`);
  });

  recording.on("saved", function(name, data) {
    const recordingId = data.id;

    pingTelemetry('recording', 'saved', {...data, recordingId});

    try {
      const browser = getBrowser();
      setRecordingSaved(browser, recordingId);
    } catch (e) {
      pingTelemetry('recording', 'save-error', {...data, recordingId, error: e});
    }

    ChromeUtils.recordReplayLog(`SavedRecording ${recordingId}`);
  });
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
  const result = await fetchText(sourceMapURL);
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
      const result = await fetchText(url);
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
    console.error(msg, mapURL, map, sourceOffset, sectionOffset);
  }

  const unresolvedSources = [];
  let sourceOffset = 0;

  if (obj.version !== 3) {
    logError("Invalid sourcemap version");
    return;
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

async function fetchText(url) {
  try {
    const response = await fetch(url);
    if (response.status < 200 || response.status >= 300) {
      console.error("Error fetching recording resource", url, response);
      return null;
    }

    return {
      url,
      text: await response.text(),
    };
  } catch (e) {
    console.error("Exception fetching recording resource", url, e);
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

function getLoggedInUserAuthId() {
  if (isRunningTest()) {
    return "auth0|5f6e41315c863800757cdf74";
  }

  const userPref = Services.prefs.getStringPref("devtools.recordreplay.user");
  if (userPref == "") {
    return;
  }

  const user = JSON.parse(userPref);
  return user == "" ? "" : user.sub;
}

Services.ppmm.addMessageListener("RecordingStarting", {
  receiveMessage(msg) {
    handleRecordingStarted(msg.target);
  },
});

// eslint-disable-next-line no-unused-vars
var EXPORTED_SYMBOLS = ["setConnectionStatusChangeCallback", "getConnectionStatus", "getDispatchServer", "isRecordingAllTabs", "isRecording", "toggleRecording", "getRecordingState", "RecordingState"];
