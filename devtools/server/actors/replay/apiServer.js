/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/* eslint-disable spaced-comment, brace-style, indent-legacy, no-shadow */

"use strict";

var EXPORTED_SYMBOLS = [
  "queryAPIServer",
  "getAPIServer"
];

const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");
const { XPCOMUtils } = ChromeUtils.import("resource://gre/modules/XPCOMUtils.jsm");
const { getenv } = ChromeUtils.import("resource://devtools/server/actors/replay/env.js");
const ReplayAuth = ChromeUtils.import("resource://devtools/server/actors/replay/auth.js");

XPCOMUtils.defineLazyGlobalGetters(this, ["fetch"]);

function getAPIServer() {
  const address = getenv("RECORD_REPLAY_API_SERVER");
  if (address) {
    return address;
  }
  return Services.prefs.getStringPref("devtools.recordreplay.apiServer");
}

async function queryAPIServer (query, variables = {}) {
  const resp = await fetch(getAPIServer(), {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      Authorization: `Bearer ${ReplayAuth.getReplayUserToken()}`,
    },
    body: JSON.stringify({
      query,
      variables
    })
  });
  
  return resp.json();
}