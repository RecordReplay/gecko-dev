# Redux DevTools Integration

The files in this directory are from the Redux DevTools ( https://github.com/reduxjs/redux-devtools ), and are loaded into recording processes so that the devtools hooks will be detected by Redux applications that have DevTools integration enabled.  The Redux DevTools logic has been hand-modified to save Redux actions data as Replay "annotations" that are sent to the driver, and from there to any clients viewing the recording.

## Files

The primary file in this folder is `page.bundle.js`.  This is a modified version of a Redux DevTools build artifact, and contains the logic that is normally loaded into pages by the actual Redux DevTools browser extension.  It defines the global variables like `window.__REDUX_DEVTOOLS_EXTENSION__ / window.__REDUX_DEVTOOLS_EXTENSION_COMPOSE__`, which application code then normally uses to talk to the extension.

`contentScript.js` imports `page.bundle.js` and initializes it by passing in a reference to `window`, so that the page file can initialize its globals.

## Updating to a newer version of Redux DevTools

Follow these steps to update `page.bundle.js` to a newer Redux DevTools version if needed.

### Clone and Build the Redux DevTools Extension

1. Clone https://github.com/reduxjs/redux-devtools
2. At the root of that repo, run `yarn` to install all dependencies.
3. At the root of that repo, run `yarn build:all` to pre-build all of the sub-packages.  This will take a few minutes.
3. Hand-edit the extension's Webpack config to allow building the bundle artifacts in non-minified form:
  - Edit `extension/webpack/base.config.js`. Uncomment `// devtool: 'source-map'` around line 10, so that Webpack generates bundle files that are plain JS instead of having all source code as large strings to be `eval()`'d.
  - Edit `extension/webpack/dev.config.babel.js`.  Comment out `config.watch = true` around line 24, so that a dev-mode build doesn't start up Webpack's watch mode.
4. In the `/extension` folder, run: `yarn webpack --config webpack/dev.config.babel.js`.  This should run Webpack, pointed at the dev-mode config.  It should output several JS bundle files into `/extension/dev/`, along with other HTML and sourcemap artifacts.
5. Inspect `/extension/dev/page.bundle.js` and confirm that the source contents are all "normal" JS, and not JS source text wrapped inside giant strings.

### Overwrite and Modify the Page File

1. Copy `/extension/dev/page.bundle.js` from the `redux-devtools` repo, and overwrite `/devtools/server/actors/replay/redux-devtools/page.bundle.js` in the `gecko-dev` repo.
2. The new `page.bundle.js` file needs several hand-edits to alter its outer IIFE wrapper and modify how it transmits actions data.  Make the following edits to the overwritten file:

#### Alter IIFE Wrapper

Replace the outer IIFE wrapper in `page.bundle.js` with a basic function that accepts `window` as an argument, then export this function as a named export:

```diff
--- a/devtools/server/actors/replay/redux-devtools/page.bundle.js
+++ b/devtools/server/actors/replay/redux-devtools/page.bundle.js
@@ -1,4 +1,5 @@
-/******/ (() => { // webpackBootstrap
+/******/ function reduxDevtoolsContentScript(window){ // webpackBootstrap


-/******/ })()
-;
+/******/ }
+
+exports.reduxDevtoolsContentScript = reduxDevtoolsContentScript
```

#### Modify Action Forwarding

Alter the `post` function to replace the `window.postMessage()` call with a call to the global Replay forwarding function that is defined over in `contentScript.js`:

```diff
@@ -2108,7 +2109,8 @@ function getSerializeParameter(config) {
 }

 function post(message) {
-  window.postMessage(message, '*');
+  window.__RECORD_REPLAY_REDUX_DEVTOOLS_SEND_BRIDGE__(message)
 }
```

#### Force Recording on Initialization

Add a line inside of the `init()` function that sends a `'START'` message synchronously and forces the DevTools processing logic to start recording store actions right away:

```diff
@@ -10360,6 +10362,9 @@ function __REDUX_DEVTOOLS_EXTENSION__(config) {
       reportId = (0,_app_stores_enhancerStore__WEBPACK_IMPORTED_MODULE_1__.getUrlParam)('remotedev_report');
       if (reportId) (0,_app_api_openWindow__WEBPACK_IMPORTED_MODULE_7__["default"])();
     }
+
+      window.postMessage({type: 'START', source: '@devtools-extension'}, '*')
   }
```

Finally, commit the altered `page.bundle.js` file.