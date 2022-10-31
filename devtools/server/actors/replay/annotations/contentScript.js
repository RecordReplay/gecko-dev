/* global chrome */

'use strict';

let window;


function initialize(dbgWindow, RecordReplayControl) {
  window = dbgWindow.unsafeDereference();

  window.wrappedJSObject.__RECORD_REPLAY_ANNOTATION_HOOK__ =
    (source, message) => {
      if (!source || typeof source !== "string") {
        window.console.error("Replay annotations must include a source");
        return false;
      }

      if (message && typeof message !== "object") {
        window.console.error("Replay annotation messages must be an object if set");
        return false;
      }

      RecordReplayControl.onAnnotation(
        "generic",
        JSON.stringify({
          source,
          message
        })
      );

      return true;
    };
}

exports.initialize = initialize;
