/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/* eslint-disable spaced-comment, brace-style, indent-legacy, no-shadow */

"use strict";

var EXPORTED_SYMBOLS = [
  "hasOriginalApiKey",
  "getOriginalApiKey",
  "setReplayUserToken",
  "getReplayUserToken",
  "refreshUserToken",
  "openSigninPage"
];

const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");
const { XPCOMUtils } = ChromeUtils.import("resource://gre/modules/XPCOMUtils.jsm");
const { setTimeout } = ChromeUtils.import("resource://gre/modules/Timer.jsm");
const { pingTelemetry } = ChromeUtils.import(
  "resource://devtools/server/actors/replay/telemetry.js"
);
const { queryAPIServer } = ChromeUtils.import(
  "resource://devtools/server/actors/replay/api-server.js"
);
const { getenv } = ChromeUtils.import(
  "resource://devtools/server/actors/replay/env.js"
);

XPCOMUtils.defineLazyServiceGetter(
  this,
  "gExternalProtocolService",
  "@mozilla.org/uriloader/external-helper-app-service;1",
  Ci.nsIExternalProtocolService
);

ChromeUtils.defineModuleGetter(
  this,
  "WebChannel",
  "resource://gre/modules/WebChannel.jsm"
);

XPCOMUtils.defineLazyGlobalGetters(this, ["fetch"]);

function defer() {
  let resolve, reject;
  const promise = new Promise(function(res, rej) {
    resolve = res;
    reject = rej;
  });
  return { resolve, reject, promise };
};

let deferredAccessToken = defer();

const Env = Cc["@mozilla.org/process/environment;1"].getService(
  Ci.nsIEnvironment
);

const gOriginalApiKey = Env.get("RECORD_REPLAY_API_KEY");
function hasOriginalApiKey() {
  return !!gOriginalApiKey;
}
function getOriginalApiKey() {
  return gOriginalApiKey;
}

function setReplayUserToken(token) {
  if (token) {
    deferredAccessToken.resolve(token);
  } else {
    deferredAccessToken = defer();
  }
  Services.prefs.setStringPref("devtools.recordreplay.user-token", token || "");
}
function getReplayUserToken() {
  return Services.prefs.getStringPref("devtools.recordreplay.user-token");
}

async function refreshUserToken() {
  const token = getReplayUserToken();

  if (token) {
    const viewHost = getenv("RECORD_REPLAY_VIEW_HOST") || "https://app.replay.io";
    const url = `${viewHost}/api/browser/refresh`;

    const resp = await fetch(url, {
      method: "POST",
      body: JSON.stringify({
        token
      })
    });

    const json = await resp.json();

    if (json.token) {
      setReplayUserToken(json.token);

      return true;
    }
  }

  return false;
}

let authChannel;

function handleAuthChannelMessage(_id, message, target) {
  const { type } = message;
  // TODO [ryanjduffy]: Add support for app login to use the browser auth flow
  // by extending this logic to support a webchannel message from the client
  // (the app) to request a login which would launch the sign in page in the
  // user's preferred browser.
  if (type === "connect") {
    deferredAccessToken.promise.then(token => {
      if (authChannel) {
        authChannel.send({ token }, target);
      }
    })
  } else if ('token' in message) {
    setReplayUserToken(message.token);
  }
}

function initializeRecordingWebChannel() {
  const pageUrl = Services.prefs.getStringPref(
    "devtools.recordreplay.recordingsUrl"
  );
  const localUrl = "http://localhost:8080/";

  registerWebChannel(pageUrl);
  registerWebChannel(localUrl);

  function registerWebChannel(url) {
    const urlForWebChannel = Services.io.newURI(url);
    authChannel = new WebChannel("record-replay-token", urlForWebChannel);

    authChannel.listen(handleAuthChannelMessage);
  }
}

function base64URLEncode(str) {
  // https://auth0.com/docs/authorization/flows/call-your-api-using-the-authorization-code-flow-with-pkce
  return str.replace(/\+/g, "-").replace(/\//g, "_").replace(/=/g, "");
}

function openSigninPage() {
  const keyArray = Array.from({length: 32}, () => String.fromCodePoint(Math.floor(Math.random() * 256)));
  const key = base64URLEncode(btoa(keyArray.join("")));
  const viewHost = getenv("RECORD_REPLAY_VIEW_HOST") || "https://app.replay.io";
  const url = Services.io.newURI(`${viewHost}/api/browser/auth?key=${key}`);

  gExternalProtocolService
    .getProtocolHandlerInfo("https")
    .launchWithURI(url);

  Promise.race([
    new Promise((_resolve, reject) => setTimeout(reject, 2 * 60 * 1000)),
    new Promise(async (resolve, reject) => {
      let retries = 0;
      while (retries < 40) {
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
          await new Promise(resolve => setTimeout(resolve, 3000));
        } else {
          setReplayUserToken(resp.data.closeAuthRequest.token);
          resolve();
          break;
        }
      }

      pingTelemetry("browser", "auth-request-failed", {
        message: "max-retries"
      });
      reject(`Failed to authenticate`);
    })
  ]).catch(console.error);
}

// Init
(() => {
  initializeRecordingWebChannel();
  
  // TODO: validate token is still okay
})();
