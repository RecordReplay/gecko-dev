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

  // The hook script uses the replay-content:// protocol so it won't be ignored
  // and clients can inspect state in its frames.
  const reactDevtoolsHookScriptURL = "replay-content:///react-devtools-hook-script";

  const { installHook } = require("devtools/server/actors/replay/react-devtools/hook");
  dbgWindow.executeInGlobal(`(${installHook}(window))`, { url: reactDevtoolsHookScriptURL });

  const { reactDevtoolsBackend } = require("devtools/server/actors/replay/react-devtools/react_devtools_backend");
  dbgWindow.executeInGlobal(`(${reactDevtoolsBackend}(window))`);

  sayHelloToBackend();
}

exports.initialize = initialize;
