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
  "openSigninPage",
  "getAuthHost"
];

const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");
const { XPCOMUtils } = ChromeUtils.import("resource://gre/modules/XPCOMUtils.jsm");
const { setTimeout, clearTimeout } = ChromeUtils.import("resource://gre/modules/Timer.jsm");
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

const Env = Cc["@mozilla.org/process/environment;1"].getService(
  Ci.nsIEnvironment
);

let lastAuthId = undefined;
let refreshTimer = null;

const gOriginalApiKey = Env.get("RECORD_REPLAY_API_KEY");
function hasOriginalApiKey() {
  return !!gOriginalApiKey;
}
function getOriginalApiKey() {
  return gOriginalApiKey;
}

function getAuthHost() {
  return getenv("RECORD_REPLAY_AUTH_HOST") || "webreplay.us.auth0.com";
}

function getAuthClientId() {
  return getenv("RECORD_REPLAY_AUTH_CLIENT_ID") || "4FvFnJJW4XlnUyrXQF8zOLw6vNAH1MAo";
}

function setReplayRefreshToken(token) {
  Services.prefs.setStringPref("devtools.recordreplay.refresh-token", token || "");
}

function getReplayRefreshToken() {
  return Services.prefs.getStringPref("devtools.recordreplay.refresh-token", "");
}

/**
 * @param {string | null} token
 */
function setReplayUserToken(token) {
  token = token || "";

  // Only update and notify if the token has changed
  if (token === getReplayUserToken()) return;

  Services.prefs.setStringPref("devtools.recordreplay.user-token", token);
  captureLastAuthId();
}
function getReplayUserToken() {
  return Services.prefs.getStringPref("devtools.recordreplay.user-token");
}

function captureLastAuthId() {
  const token = getReplayUserToken();
  if (token) {
    const t = tokenInfo(token);
    lastAuthId = t?.payload.sub;
  }
}

/**
 * @param {string} token
 * @returns {{payload: Record<string, unknown>} | null}
 */
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

/**
 * @param {string} token
 * @returns {number | null}
 */
function tokenExpiration(token) {
  const userInfo = tokenInfo(token);
  if (!userInfo) {
    return null;
  }
  const exp = userInfo.payload?.exp;
  return typeof exp === "number" ? exp * 1000 : null;
}

function scheduleRefreshTimer(expiresInMs) {
  if (refreshTimer) {
    clearTimeout(refreshTimer);
  }

  // refresh a minute before token expiration
  refreshTimer = setTimeout(refresh, expiresInMs - (60 * 1000));
}

/**
 * @returns {Promise<string | null>}
 */
async function validateUserToken() {
  const userToken = getReplayUserToken();
  const userTokenInfo = tokenInfo(userToken);

  if (!userToken || !userTokenInfo) {
    return null;
  }

  const exp = tokenExpiration(userToken);
  if (exp > Date.now()) {
    // The current token hasn't expired yet so schedule a refresh and return it
    scheduleRefreshTimer(exp - Date.now());

    return userToken;
  }

  if (!getReplayRefreshToken()) {
    pingTelemetry("browser", "no-refresh-token", {
      expirationDate: new Date(exp).toISOString(),
      authId: userTokenInfo.payload.sub
    });

    return;
  }

  try {
    return await refresh();
  } catch (e) {
    handleAuthRequestFailed(e);
  }
}

// Tracks the open replay.io tabs. If one is closed, its currentWindowGlobal
// will be set to null and will be removed from the map on the next pass
const webChannelTargets = new Map();

// Notifies all replay.io tabs of the current auth state
function notifyWebChannelTargets() {
  for (let [key, {channel, target}] of Array.from(webChannelTargets.entries())) {
    if (target.browsingContext.currentWindowGlobal) {
      notifyWebChannelTarget(channel, target);
    } else {
      webChannelTargets.delete(key);
    }
  }
}

// Notify a single tab of the current auth state
async function notifyWebChannelTarget(channel, target) {
  const token = await validateUserToken();

  if (token) {
    channel.send({ token }, target);
  }
}

function handleAuthChannelMessage(channel, _id, message, target) {
  const { type } = message;
  if (type === "login") {
    openSigninPage(target.browser);
  } else if (type === "connect") {
    webChannelTargets.set(target.browsingContext, {channel, target});
    notifyWebChannelTarget(channel, target);
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
    const channel = new WebChannel("record-replay-token", urlForWebChannel);

    channel.listen((...args) => handleAuthChannelMessage(channel, ...args));
  }
}

function handleAuthRequestFailed(e, extra = {}) {
  console.error(e);

  const message = e?.id || "unexpected-internal-error";
  const errorMessage = e?.message;

  switch (message) {
    case 'timeout':
      showAuthenticationError("The request timed out before authentication completed. Please try again.")
      break;
    case 'unexpected-internal-error':
      showAuthenticationError("An unexpected error occurred. Support has been notified. You may try again or contact support@replay.io for help.");
      break;
  }

  pingTelemetry("browser", "auth-request-failed", {
    message,
    errorMessage,
    authId: lastAuthId,
    ...extra,
  });
}

async function refresh() {
  try {
    const resp = await fetch(`https://${getAuthHost()}/oauth/token`, {
      method: "POST",
      headers: {"Content-Type": "application/json"},
      body: JSON.stringify({
        audience: "https://api.replay.io",
        scope: "openid profile offline_access",
        grant_type: "refresh_token",
        client_id: getAuthClientId(),
        refresh_token: getReplayRefreshToken(),
      })
    });

    const json = await resp.json();

    if (json.error) {
      throw {
        id: "auth0-error",
        message: json.error
      };
    }

    if (!json.access_token) {
      throw {
        id: "no-access-token"
      };
    }

    if (!json.refresh_token) {
      throw {
        id: "no-refresh-token"
      };
    }

    setReplayRefreshToken(json.refresh_token);
    setReplayUserToken(json.access_token);

    scheduleRefreshTimer(json.expires_in * 1000);

    return json.access_token;
  } catch (e) {
    setReplayRefreshToken("");
    setReplayUserToken("");

    throw e;
  }
}

function showAuthenticationError(message) {
  try {
    const mainWindow = Services.wm.getMostRecentWindow("navigator:browser");
    const browser = mainWindow.gBrowser.selectedBrowser;
    const notificationKey = "replay-invalidated-recording";
    const notificationBox = browser.getTabBrowser().getNotificationBox(browser);
    const notification = notificationBox.getNotificationWithValue(notificationKey);

    if (notification) {
      return;
    }

    notificationBox.appendNotification(
      message,
      notificationKey,
      undefined,
      notificationBox.PRIORITY_WARNING_HIGH,
    );
  } catch (e) {
    console.error(e);
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

  let timedOut = false;
  Promise.race([
    new Promise((_resolve, reject) => setTimeout(() => {
      timedOut = true;
      reject({id: "timeout"});
    }, 2 * 60 * 1000)),
    new Promise(async (resolve, reject) => {
      try {
        while (!timedOut) {
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
            if (resp.errors.length === 1 && resp.errors[0].message === "Authentication request does not exist") {
              await new Promise(resolve => setTimeout(resolve, 3000));
            } else {
              throw {
                id: "close-graphql-error",
                message: resp.errors.map(e => e.message).filter(Boolean).join(", ")
              };
            }
          } else if (!resp.data.closeAuthRequest.token) {
            // there's no obvious reason this would occur but for completeness ...
            throw {
              id: "close-missing-token",
              message: JSON.stringify(resp)
            };
          } else {
            setReplayRefreshToken(resp.data.closeAuthRequest.token);

            // refresh the token immediately to exchange it for an access token
            // and acquire a new refresh token
            await refresh();

            resolve();
            break;
          }
        }
      } catch (e) {
        reject(e);
      }
    })
  ]).catch(e => {
    handleAuthRequestFailed(e, {clientKey: key})
  });
}

Services.prefs.addObserver("devtools.recordreplay.user-token", () => {
  notifyWebChannelTargets();
});

// Init
(async () => {
  initializeRecordingWebChannel();
  if (!hasOriginalApiKey()) {
    captureLastAuthId();
    // clear the token so we ensure that the token is communicated to connection.js
    const token = await validateUserToken();
    setReplayUserToken(null);
    setReplayUserToken(token);
  }
})();
