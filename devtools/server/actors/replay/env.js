/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/* eslint-disable spaced-comment, brace-style, indent-legacy, no-shadow */

"use strict";

var EXPORTED_SYMBOLS = [
  "getenv",
  "setenv"
];

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
