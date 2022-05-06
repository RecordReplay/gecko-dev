/* global chrome */

'use strict';

let window;


function initialize(dbgWindow, RecordReplayControl) {
  dump(`REDUX_DEVTOOLS_INITIALIZE\n`);

  try {
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
  } catch (e) {
    dump(`REDUX_DEVTOOLS_EXCEPTION ${e}\n`);
  }
}

exports.initialize = initialize;
