# React Devtools Integration

The files in this directory are from the React Devtools (https://github.com/facebook/react/tree/master/packages/react-devtools), and are loaded into recording processes so that the devtools hooks will be detected by any React application on the page and allow events to be sent to the driver and from there on to any clients viewing the recording.

From the base React revision [`adb8ebc927ea091ba5ffba6a9f30dbe62eaee0c5`](https://github.com/facebook/react/commit/adb8ebc927ea091ba5ffba6a9f30dbe62eaee0c5), the other files are as follows:

### contentScript.js

Modified from (react) [`packages/react-devtools-extensions/src/contentScript.js`](https://github.com/facebook/react/blob/main/packages/react-devtools-extensions/src/contentScript.js).

### hook.js

Modified from (react) [`packages/react-devtools-shared/src/hook.js`](https://github.com/facebook/react/blob/main/packages/react-devtools-shared/src/hook.js).

### react_devtools_backend.js

After building React, this is modified from the generated file `packages/react-devtools-extensions/firefox/build/unpacked/build/react_devtools_backend.js` with the patch below.

```diff
@@ -1,3 +1,5 @@
+function reactDevtoolsBackend(window) {
+
 /******/ (function(modules) { // webpackBootstrap
 /******/    // The module cache
 /******/    var installedModules = {};
@@ -11462,6 +11464,18 @@ class agent_Agent extends events["a" /* default */] {
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
@@ -12946,7 +12960,7 @@ function getStackByFiberInDevAndProd(workTagMap, workInProgress, currentDispatch
 let welcomeHasInitialized = false;
 
 function welcome(event) {
-  if (event.source !== window || event.data.source !== 'react-devtools-content-script') {
+  if (event.data.source !== 'react-devtools-content-script') {
     return;
   } // In some circumstances, this method is called more than once for a single welcome message.
   // The exact circumstances of this are unclear, though it seems related to 3rd party event batching code.
@@ -13005,13 +13019,8 @@ function setup(hook) {
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
@@ -16548,4 +16557,8 @@ function setStyle(agent, id, rendererID, name, value) {
 }
 
 /***/ })
-/******/ ]);
+/******/ ]);
+
+}
+
+exports.reactDevtoolsBackend = reactDevtoolsBackend;
```

## Updating to a newer version of React Devtools

1. Clone the React repository and build the firefox extension:
```sh
git clone https://github.com/facebook/react.git

cd react

# Install build dependencies.
yarn install

cd scripts/release/

# Install release script dependencies.
yarn install

cd ../../

# Download the latest React release (built from CI).
# You can build this locally too but it's much slower.
scripts/release/download-experimental-build.js --commit=main

cd packages/react-devtools-extensions/

# Build the Firefox extension
yarn build:firefox
```

2. Copy `packages/react-devtools-extensions/firefox/build/unpacked/build/react_devtools_backend.js` to this folder and apply the modifications from the patch above.

3. Check if there have been any changes in `packages/react-devtools-shared/src/hook.js` since the last update and apply them to the file in this folder.

4. Update the React revision and the patch in this file
