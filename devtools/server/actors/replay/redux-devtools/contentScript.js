/* global chrome */

'use strict';

let window;


function initialize(dbgWindow, RecordReplayControl) {
  window = dbgWindow.unsafeDereference();

  window.wrappedJSObject.__RECORD_REPLAY_REDUX_DEVTOOLS_SEND_BRIDGE__ =
    (message) => {
      RecordReplayControl.onAnnotation(
        "redux-devtools-bridge",
        JSON.stringify(message )
      );
    };

  const { reduxDevtoolsContentScript } = require("devtools/server/actors/replay/redux-devtools/page.bundle");
  dbgWindow.executeInGlobal(`(${reduxDevtoolsContentScript}(window))`);
}

exports.initialize = initialize;
