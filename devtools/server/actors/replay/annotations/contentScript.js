/* global chrome */

'use strict';

let window;


function initialize(dbgWindow, RecordReplayControl) {
  window = dbgWindow.unsafeDereference();

  window.wrappedJSObject.__RECORD_REPLAY_ANNOTATION_HOOK__ =
    (source, message) => {
      RecordReplayControl.onAnnotation(
        "generic-annotation",
        JSON.stringify({
          source,
          message
        })
      );
    };
}

exports.initialize = initialize;
