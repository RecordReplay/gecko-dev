/* global chrome */

'use strict';

let window;


function initialize(dbgWindow, RecordReplayControl) {
  window = dbgWindow.unsafeDereference();

  window.wrappedJSObject.__RECORD_REPLAY_ANNOTATION_HOOK__ =
    (kind, message) => {
      if (!kind || typeof kind !== "string") {
        window.console.error("Replay annotation `kind` must be a string");
        return false;
      }

      RecordReplayControl.onAnnotation(
        kind,
        JSON.stringify({
          message
        })
      );

      return true;
    };
}

exports.initialize = initialize;
