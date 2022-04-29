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
        JSON.stringify({ event, payload, stack: Error().stack })
      );
    };

  window.wrappedJSObject.__RECORD_REPLAY_REMEMBER_OBJECT__ = obj => {
    RecordReplayControl.ensurePersistentId(obj);
  };

  window.wrappedJSObject.__RECORD_REPLAY_ANNOTATE__ = annotation => {
    RecordReplayControl.onAnnotation(annotation, "");
  };

  window.wrappedJSObject.__RECORD_REPLAY_LOG__ = RecordReplayControl.log;

  const { installHook } = require("devtools/server/actors/replay/react-devtools/hook");
  dbgWindow.executeInGlobal(`(${installHook}(window))`);

  const { reactDevtoolsBackend } = require("devtools/server/actors/replay/react-devtools/react_devtools_backend");
  dbgWindow.executeInGlobal(`(${reactDevtoolsBackend}(window))`);

  RecordReplayControl.onAnnotation("react-devtools-initialize", "");

  sayHelloToBackend();
}

exports.initialize = initialize;
