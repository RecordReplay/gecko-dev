/* global chrome */

'use strict';

let window;

function sayHelloToBackend() {
  window.postMessage(
    {
      source: 'react-devtools-content-script',
      hello: true,
    },
    '*',
  );
}

function initialize(dbgWindow, RecordReplayControl) {
  window = dbgWindow.unsafeDereference();

  window.wrappedJSObject.__RECORD_REPLAY_REACT_DEVTOOLS_SEND_BRIDGE__ =
    (event, payload) => {
      RecordReplayControl.onAnnotation(
        "react-devtools-bridge",
        JSON.stringify({ event, payload })
      );
    };

  window.wrappedJSObject.__RECORD_REPLAY_REACT_DEVTOOLS_HOOK__ = kind => {
    RecordReplayControl.onAnnotation("react-devtools-hook", kind);
  };

  window.wrappedJSObject.__RECORD_REPLAY_PERSISTENT_ID__ = obj => {
    return RecordReplayControl.getPersistentId(obj);
  };

  // The hook script is given a special URL so it won't be ignored and clients can inspect
  // state in its frames.
  const { installHook } = require("devtools/server/actors/replay/react-devtools/hook");
  dbgWindow.executeInGlobal(`(${installHook}(window))`, { url: "react-devtools-hook-script" });

  const { reactDevtoolsBackend } = require("devtools/server/actors/replay/react-devtools/react_devtools_backend");
  dbgWindow.executeInGlobal(`(${reactDevtoolsBackend}(window))`);

  sayHelloToBackend();
}

exports.initialize = initialize;
