/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/* eslint-disable spaced-comment, brace-style, indent-legacy, no-shadow */

"use strict";

var EXPORTED_SYMBOLS = [
  "getApiKey",
  "getViewHost"
];

const { getenv } = ChromeUtils.import(
  "resource://devtools/server/actors/replay/env.js"
);

function getViewHost() {
  return getenv("RECORD_REPLAY_VIEW_HOST") || "https://app.replay.io";
}

function getApiKey() {
  return getenv("RECORD_REPLAY_API_KEY");
}
