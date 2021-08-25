# React Devtools Integration

The files in this directory are from the React Devtools (https://github.com/facebook/react/tree/master/packages/react-devtools), and are loaded into recording processes so that the devtools hooks will be detected by any React application on the page and allow events to be sent to the driver and from there on to any clients viewing the recording.

From the base React revision b9964684bd8c909fc3d88f1cd47aa1f45ea7ba32, the other files are as follows:

### contentScript.js

Modified from `react/packages/react-devtools-extensions/src/contentScript.js`

### hook.js

Modified from `packages/react-devtools-shared/src/hook.js`

### react_devtools_backend.js

After building React, this is modified from the generated file `packages/react-devtools-extensions/firefox/build/unpacked/build/react_devtools_backend.js` with the patch below.

```
@@ -1,3 +1,5 @@
+function reactDevtoolsBackend(window) {
+
 /******/ (function(modules) { // webpackBootstrap
 /******/ 	// The module cache
 /******/ 	var installedModules = {};
@@ -10498,6 +10500,18 @@
     bridge.send('isSynchronousXHRSupported', Object(utils["h" /* isSynchronousXHRSupported */])());
     setupHighlighter(bridge, this);
     TraceUpdates_initialize(this);
+
+    // Hook for sending messages via record/replay evaluations.
+    window.__RECORD_REPLAY_REACT_DEVTOOLS_SEND_MESSAGE__ = (inEvent, inData) => {
+      let rv;
+      this._bridge = {
+        send(event, data) {
+          rv = { event, data };
+        }
+      };
+      this[inEvent](inData);
+      return rv;
+    };
   }
 
   get rendererInterfaces() {
@@ -11444,7 +11458,7 @@
 
 
 function welcome(event) {
-  if (event.source !== window || event.data.source !== 'react-devtools-content-script') {
+  if (event.data.source !== 'react-devtools-content-script') {
     return;
   }
 
@@ -11487,13 +11501,8 @@
     },
 
     send(event, payload, transferable) {
-      window.postMessage({
-        source: 'react-devtools-bridge',
-        payload: {
-          event,
-          payload
-        }
-      }, '*', transferable);
+      // Synchronously notify the record/replay driver.
+      window.__RECORD_REPLAY_REACT_DEVTOOLS_SEND_BRIDGE__(event, payload);
     }
 
   });
@@ -15243,4 +15252,8 @@
 }
 
 /***/ })
 /******/ ]);
+
+}
+
+exports.reactDevtoolsBackend = reactDevtoolsBackend;
```

## Updating to a newer version of React Devtools

* clone the React repository and build the firefox extension: 
  ```
  git clone https://github.com/facebook/react.git
  cd react
  yarn
  yarn build-for-devtools
  cd packages/react-devtools-extensions/
  yarn build:firefox
  ```
* copy `packages/react-devtools-extensions/firefox/build/unpacked/build/react_devtools_backend.js` to this folder and apply the modifications from the patch above
* check if there have been any changes in `packages/react-devtools-shared/src/hook.js` since the last update and apply them to the file in this folder
* update the React revision and the patch in this file
