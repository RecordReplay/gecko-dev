/* global chrome */

'use strict';

let window;


function initialize(dbgWindow, RecordReplayControl) {
  window = dbgWindow.unsafeDereference();

  window.wrappedJSObject.__RECORD_REPLAY_ANNOTATION_HOOK__ =
    (source, message) => {
      if (!source || typeof source !== "string") {
        throw new Error("Replay annotations must include a source");
      }

      if (message && typeof message !== "object") {
        throw new Error("Replay annotation messages must be an object if set");
      }

      RecordReplayControl.onAnnotation(
        "generic",
        JSON.stringify({
          source,
          message
        })
      );
    };
}

exports.initialize = initialize;
