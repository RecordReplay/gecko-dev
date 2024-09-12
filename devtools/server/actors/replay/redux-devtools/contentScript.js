/* global chrome */

'use strict';

let window;


function initialize(dbgWindow, RecordReplayControl) {
  window = dbgWindow.unsafeDereference();

  const { reduxDevtoolsContentScript } = require("devtools/server/actors/replay/redux-devtools/page.bundle");
  dbgWindow.executeInGlobal(`(${reduxDevtoolsContentScript}(window))`);
}

exports.initialize = initialize;
