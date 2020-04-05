/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/* eslint-disable spaced-comment, brace-style, indent-legacy, no-shadow */

"use strict";

const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");
const { XPCOMUtils } = ChromeUtils.import("resource://gre/modules/XPCOMUtils.jsm");
const { OS } = ChromeUtils.import("resource://gre/modules/osfile.jsm");
const { setTimeout } = Components.utils.import('resource://gre/modules/Timer.jsm');

XPCOMUtils.defineLazyModuleGetters(this, {
  AppUpdater: "resource:///modules/AppUpdater.jsm",
});

// This file provides an interface for connecting middleman processes with
// replaying processes living remotely in the cloud.

// Worker which handles the sockets connecting to remote processes.
let gWorker;

// Callbacks supplied on startup.
let gCallbacks;

// Next ID to use for a replaying process connection.
let gNextConnectionId = 1;

// eslint-disable-next-line no-unused-vars
function Initialize(address, callbacks) {
  gWorker = new Worker("connection-worker.js");
  gWorker.addEventListener("message", evt => {
    try {
      onMessage(evt);
    } catch (e) {
      ChromeUtils.recordReplayLog(`RecordReplaySocketError ${e}`);
    }
  });
  gCallbacks = callbacks;

  const buildId = `macOS-${Services.appinfo.appBuildID}`;
  const verbose = !!getenv("WEBREPLAY_VERBOSE");

  gWorker.postMessage({ kind: "initialize", address, buildId, verbose });
}

// ID assigned to this browser session by the cloud server.
let gSessionId;

function onMessage(evt) {
  switch (evt.data.kind) {
    case "updateStatus":
      gCallbacks.updateStatus(evt.data.status);
      break;
    case "loaded": {
      let { sessionId, controlJS, replayJS, updateNeeded, updateWanted } = evt.data;

      dump(`DispatcherSessionId ${sessionId}\n`);

      gSessionId = sessionId;
      flushOfflineLog();

      if (updateNeeded) {
        gCallbacks.updateStatus("cloudUpdateNeeded.label");
      } else {
        gCallbacks.updateStatus("");
        gCallbacks.loadedJS(controlJS, replayJS);
      }

      if (updateNeeded || updateWanted) {
        downloadUpdate(updateNeeded);
      }
      break;
    }
    case "connectionFailed":
      Services.cpmm.sendAsyncMessage("RecordReplayCriticalError", { kind: "CloudSpawnError" });
      break;
    case "connected":
      gCallbacks.onConnected(evt.data.id);
      break;
    case "disconnected":
      gCallbacks.onDisconnected(evt.data.id);
      break;
    case "error":
      if (evt.data.id) {
        gCallbacks.onDisconnected(evt.data.id);
      }
      break;
    case "logOffline":
      addToOfflineLog(evt.data.text);
      break;
  }
}

// eslint-disable-next-line no-unused-vars
function Connect(channelId) {
  dump(`RecordReplayConnect\n`);
  const id = gNextConnectionId++;
  gWorker.postMessage({ kind: "connect", id, channelId });
  return id;
}

// eslint-disable-next-line no-unused-vars
function AddToLog(text) {
  gWorker.postMessage({ kind: "log", text });
}

let gAppUpdater;

function downloadStatusListener(status, ...args) {
  switch (status) {
    case AppUpdater.STATUS.READY_FOR_RESTART:
      gCallbacks.updateStatus("cloudUpdateDownloaded.label");
      break;
    case AppUpdater.STATUS.OTHER_INSTANCE_HANDLING_UPDATES:
    case AppUpdater.STATUS.CHECKING:
    case AppUpdater.STATUS.STAGING:
      gCallbacks.updateStatus("cloudUpdateDownloading.label");
      break;
    case AppUpdater.STATUS.DOWNLOADING:
      if (!args.length) {
        gCallbacks.updateStatus("cloudUpdateDownloading.label",
                                0, gAppUpdater.update.selectedPatch.size);
      } else {
        const [progress, max] = args;
        gCallbacks.updateStatus("cloudUpdateDownloading.label", progress, max);
      }
      break;
    case AppUpdater.STATUS.UPDATE_DISABLED_BY_POLICY:
    case AppUpdater.STATUS.NO_UPDATES_FOUND:
    case AppUpdater.STATUS.UNSUPPORTED_SYSTEM:
    case AppUpdater.STATUS.MANUAL_UPDATE:
    case AppUpdater.STATUS.DOWNLOAD_AND_INSTALL:
    case AppUpdater.STATUS.DOWNLOAD_FAILED:
      gCallbacks.updateStatus("cloudUpdateManualDownload.label");
      break;
  }
}

function getenv(name) {
  const env = Cc["@mozilla.org/process/environment;1"].getService(Ci.nsIEnvironment);
  return env.get(name);
}

function downloadUpdate(updateNeeded) {
  // Allow connecting to the cloud with an unknown build.
  if (getenv("WEBREPLAY_NO_UPDATE")) {
    gCallbacks.updateStatus("");
    return;
  }

  if (gAppUpdater) {
    return;
  }
  gAppUpdater = new AppUpdater();
  if (updateNeeded) {
    gAppUpdater.addListener(downloadStatusListener);
  }
  gAppUpdater.check();
}

function offlineLogPath() {
  let dir = Services.dirsvc.get("UAppData", Ci.nsIFile);
  dir.append("Recordings");

  if (!dir.exists()) {
    OS.File.makeDir(dir.path);
  }

  dir.append("offlineLog.log");
  return dir.path;
}

// If defined, this reflects the full contents of the offline log.
let offlineLogContents;
let hasOfflineLogFlushTimer;

async function waitForOfflineLogContents() {
  if (offlineLogContents !== undefined) {
    return;
  }

  const path = offlineLogPath();

  if (!(await OS.File.exists(path))) {
    offlineLogContents = "";
    return;
  }

  const file = await OS.File.read(path);
  offlineLogContents = new TextDecoder("utf-8").decode(file);
}

async function addToOfflineLog(text) {
  await waitForOfflineLogContents();
  offlineLogContents += `Offline ${gSessionId} ${text}`;

  if (!hasOfflineLogFlushTimer) {
    hasOfflineLogFlushTimer = true;
    setTimeout(() => {
      if (offlineLogContents.length) {
        OS.File.writeAtomic(offlineLogPath(), offlineLogContents);
      }
      hasOfflineLogFlushTimer = false;
    }, 500);
  }
}

async function flushOfflineLog() {
  await waitForOfflineLogContents();

  if (offlineLogContents.length) {
    AddToLog(offlineLogContents);
    offlineLogContents = "";
    OS.File.remove(offlineLogPath());
  }
}

// eslint-disable-next-line no-unused-vars
var EXPORTED_SYMBOLS = ["Initialize", "Connect", "AddToLog"];
