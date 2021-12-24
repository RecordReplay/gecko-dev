/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/* eslint-disable spaced-comment, brace-style, indent-legacy, no-shadow */

"use strict";

var EXPORTED_SYMBOLS = [
  "hasOriginalApiKey",
  "getOriginalApiKey",
  "setReplayRefreshToken",
  "setReplayUserToken",
  "getReplayUserToken",
  "tokenInfo",
  "tokenExpiration",
  "openSigninPage"
];

const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");
const { XPCOMUtils } = ChromeUtils.import("resource://gre/modules/XPCOMUtils.jsm");
const { setTimeout } = ChromeUtils.import("resource://gre/modules/Timer.jsm");
const { queryAPIServer } = ChromeUtils.import(
  "resource://devtools/server/actors/replay/api-server.js"
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

function setReplayRefreshToken(token) {
  Services.prefs.setStringPref("devtools.recordreplay.refresh-token", token || "");
  refresh(token);
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

function tokenInfo(token) {
  const [_header, encPayload, _cypher] = token.split(".", 3);
  if (typeof encPayload !== "string") {
    return null;
  }

  let payload;
  try {
    const decPayload = ChromeUtils.base64URLDecode(encPayload, {
      padding: "reject"
    });
    payload = JSON.parse(new TextDecoder().decode(decPayload));
  } catch (err) {
    return null;
  }

  if (typeof payload !== "object") {
    return null;
  }

  return { payload };
}

function tokenExpiration(token) {
  const userInfo = tokenInfo(token);
  if (!userInfo) {
    return null;
  }
  const exp = userInfo.payload?.exp;
  return typeof exp === "number" ? exp * 1000 : null;
}

function handleAuthChannelMessage(_id, message, target) {
  const { type } = message;
  if (type === "connect") {
    deferredAccessToken.promise.then(token => {
      authChannel.send({ token }, target);
    })
  // TODO [ryanjduffy]: Add support for app login to use the browser auth flow
  // } else if (type === "login") {
  //   openSigninPage();
  } else if ('token' in message) {
    if (!message.token) {
      setReplayRefreshToken(null);
    }
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
    const authChannel = new WebChannel("record-replay-token", urlForWebChannel);

    authChannel.listen(handleAuthChannelMessage);
  }
}

async function refresh(refreshToken) {
  if (!refreshToken) {
    return;
  }

  const resp = await fetch("https://webreplay.us.auth0.com/oauth/token", {
    method: "POST",
    headers: {"Content-Type": "application/json"},
    body: JSON.stringify({
      audience: "https://api.replay.io",
      scope: "openid profile offline_access",
      grant_type: "refresh_token",
      client_id: "4FvFnJJW4XlnUyrXQF8zOLw6vNAH1MAo",
      refresh_token: refreshToken,
    })
  });

  const json = await resp.json();

  if (json.error) {
    // TODO: telemetry
    setReplayRefreshToken("");
    setReplayUserToken("");
    return;
  }


  if (json.access_token) {
    Services.prefs.setStringPref("devtools.recordreplay.refresh-token", json.refresh_token);
    setReplayUserToken(json.access_token);

    setTimeout(refresh, json.expires_in * 1000);
  }
}

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
          setReplayRefreshToken(resp.data.closeAuthRequest.token);
          resolve();
          break;
        }
      }

      reject(`Failed to authenticate`);
    })
  ]).catch(console.error);
}


// Init
(() => {
  initializeRecordingWebChannel();
  refresh(Services.prefs.getStringPref("devtools.recordreplay.refresh-token", ""));
})();
