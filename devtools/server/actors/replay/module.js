/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Logic that always runs in recording/replaying processes, and which can affect the recording.

// Create a sandbox with resources from Gecko components we need.
const sandbox = Cu.Sandbox(
  Components.Constructor("@mozilla.org/systemprincipal;1", "nsIPrincipal")(),
  {
    wantGlobalProperties: ["InspectorUtils", "CSSRule"],
  }
);
Cu.evalInSandbox(
  `
Components.utils.import('resource://gre/modules/jsdebugger.jsm');
Components.utils.import('resource://gre/modules/Services.jsm');
addDebuggerToGlobal(this);
`,
  sandbox
);
const { Debugger, RecordReplayControl, Services, InspectorUtils } = sandbox;

// This script can be loaded into non-recording/replaying processes during automated tests.
// In non-recording/replaying processes there are no properties on RecordReplayControl.
const isRecordingOrReplaying = !!RecordReplayControl.onNewSource;

if (isRecordingOrReplaying) {
  Services.cpmm.sendAsyncMessage("RecordingStarting");
}

const log = RecordReplayControl.log;

const { CryptoUtils } = ChromeUtils.import(
  "resource://services-crypto/utils.js"
);

const {
  getChannelRequestData,
  getChannelRequestDoneData,
  getChannelRequestFailedData,
} = ChromeUtils.import(
  "resource://devtools/server/actors/replay/network-helpers.jsm"
);

const { NetUtil } = ChromeUtils.import("resource://gre/modules/NetUtil.jsm");

const { ComponentUtils } = ChromeUtils.import("resource://gre/modules/ComponentUtils.jsm");

const { require } = ChromeUtils.import("resource://devtools/shared/Loader.jsm");

const { getCurrentZoom } = require("devtools/shared/layout/utils");
const { getDebuggerSourceURL } = require("devtools/server/actors/utils/source-url");

const {
  initialize: initReactDevtools,
} = require("devtools/server/actors/replay/react-devtools/contentScript");

const { StackingContext } = require("devtools/server/actors/replay/stacking-context");

let gWindow;
function getWindow() {
  if (!gWindow) {
    for (const w of Services.ww.getWindowEnumerator()) {
      gWindow = w;
      break;
    }
  }
  return gWindow;
}

function getWindowAsImageData(win) {
  const canvas = win.document.createElementNS(
    "http://www.w3.org/1999/xhtml",
    "canvas"
  );
  const scale = getCurrentZoom(win);
  const width = win.innerWidth;
  const height = win.innerHeight;
  canvas.width = width * scale;
  canvas.height = height * scale;
  canvas.mozOpaque = true;

  const ctx = canvas.getContext("2d");

  ctx.scale(scale, scale);
  ctx.drawWindow(win, win.scrollX, win.scrollY, width, height, "#fff");

  const dataURL = canvas.toDataURL("image/jpeg", 0.5);
  return dataURL.slice(dataURL.indexOf(",") + 1);
}

const startTime = new Date();
const gDebugger = new Debugger();
const gSandboxGlobal = gDebugger.makeGlobalObjectReference(sandbox);
const gAllGlobals = [];

function considerScript(script) {
  return script.format == "js" &&
    RecordReplayControl.shouldUpdateProgressCounter(script.url) &&
    // Ignore default class constructors. These are cloned from self hosted
    // scripts and then marked as not self hosted so the debugger can see them.
    // They won't have instrumentation, though, and we need instrumentation
    // for script enters/exits to be consistent with the stack contents.
    !script.isDefaultClassConstructor;
}

// Call the callback for each frame, starting at the oldest to the newest.
function findScriptFrame(callback) {
  const frames = [];
  for (let frame = gDebugger.getNewestFrame(); frame; frame = frame.older) {
    if (considerScript(frame.script)) {
      frames.push(frame);
    }
  }

  for (let i = frames.length - 1; i >= 0; i--) {
    const frame = frames[i];
    if (callback(frame, i)) {
      return frame;
    }
  }
  return null;
}

function forEachScriptFrame(callback) {
  findScriptFrame((frame, index) => { callback(frame, index); });
}

function countScriptFrames() {
  let count = 0;
  forEachScriptFrame(() => count++);
  return count;
}

///////////////////////////////////////////////////////////////////////////////
// Utilities
///////////////////////////////////////////////////////////////////////////////

function assert(v, msg = "") {
  if (!v) {
    log(`Error: Assertion failed ${msg} ${Error().stack}`);
    throw new Error("Assertion failed!");
  }
}

function isNonNullObject(obj) {
  return obj && (typeof obj == "object" || typeof obj == "function");
}

// Bidirectional map between values and numeric IDs.
class IdMap {
  constructor() {
    this.clear();
  }

  add(obj) {
    if (this._objectMap.has(obj)) {
      return this._objectMap.get(obj);
    }
    const id = this._idMap.length;
    this._idMap.push(obj);
    this._objectMap.set(obj, id);
    return id;
  }

  getId(obj) {
    return this._objectMap.get(obj) || 0;
  }

  getObject(id) {
    return this._idMap[id];
  }

  map(callback) {
    const rv = [];
    for (let i = 1; i < this._idMap.length; i++) {
      rv.push(callback(i));
    }
    return rv;
  }

  forEach(callback) {
    for (let i = 1; i < this._idMap.length; i++) {
      callback(i, this._idMap[i]);
    }
  }

  clear() {
    this._idMap = [undefined];
    this._objectMap = new Map();
  }
}

// Map from keys to arrays of values.
class ArrayMap {
  constructor() {
    this.map = new Map();
  }

  add(key, value) {
    if (this.map.has(key)) {
      this.map.get(key).push(value);
    } else {
      this.map.set(key, [value]);
    }
  }
}

///////////////////////////////////////////////////////////////////////////////
// Main Logic
///////////////////////////////////////////////////////////////////////////////

const gSourceMapData = new WeakMap();

function setSourceMap({
  window,
  object,
  objectURL,
  objectText,
  objectMapURL: url
}) {
  if (!Services.prefs.getBoolPref("devtools.recordreplay.uploadSourceMaps")) {
    return;
  }
  const recordingId = RecordReplayControl.recordingId();
  if (!recordingId || !url) {
    return;
  }

  let sourceBaseURL;
  if (typeof objectURL === "string" && isValidBaseURL(objectURL)) {
    sourceBaseURL = objectURL;
  } else if (window?.location?.href && isValidBaseURL(window?.location?.href)) {
    sourceBaseURL = window.location.href;
  }

  let sourceMapURL
  try {
    sourceMapURL = new URL(url, sourceBaseURL).toString();
  } catch (err) {
    log("Failed to process sourcemap url: " + err.message);
    return;
  }

  // If the map was a data: URL or something along those lines, we want
  // to resolve paths in the map relative to the overall base.
  const sourceMapBaseURL =
    isValidBaseURL(sourceMapURL) ? sourceMapURL : sourceBaseURL;

  gSourceMapData.set(object, {
    url: sourceMapURL,
    baseUrl: sourceMapBaseURL
  });

  Services.cpmm.sendAsyncMessage("RecordReplayGeneratedSourceWithSourceMap", {
    recordingId,
    sourceMapURL,
    sourceMapBaseURL,

    // Do an exact match on this map's URL, so that even if the content is not
    // available or if it has no URL, we can still make a best-effort match
    // for the map. This won't be specific enough on its own if the page
    // was loaded multiple times with different maps, but that's all we can do.
    targetMapURLHash: makeAPIHash(sourceMapURL),

    // Attempt to be more specific by matching on the script's URL and content.
    targetContentHash: typeof objectText === "string" ? makeAPIHash(objectText) : undefined,
    targetURLHash: typeof objectURL === "string" ? makeAPIHash(objectURL) : undefined,
  });
}

function makeAPIHash(content) {
  assert(typeof content === "string");
  return "sha256:" + CryptoUtils.sha256(content);
}

function isValidBaseURL(url) {
  try {
    new URL("", url);
    return true;
  } catch {
    return false;
  }
}

function getSourceMapData(object) {
  return gSourceMapData.get(object);
}

const gNewGlobalHooks = [];
gDebugger.onNewGlobalObject = (global) => {
  try {
    gDebugger.addDebuggee(global);
    gAllGlobals.push(global);
    gNewGlobalHooks.forEach((hook) => hook(global));
  } catch (e) {}
};

let gExceptionValue;

gDebugger.onExceptionUnwind = (frame, value) => {
  gExceptionValue = { value };
  RecordReplayControl.onExceptionUnwind();
  gExceptionValue = null;
};

gDebugger.onDebuggerStatement = () => {
  RecordReplayControl.onDebuggerStatement();
};

// Associate each Debugger.Script with a numeric ID.
const gScripts = new IdMap();

// Associate each Debugger.Source with a numeric ID.
const gSources = new IdMap();

// Map Debugger.Source.id to Debugger.Source.
const gGeckoSources = new Map();

// Map Debugger.Source to arrays of the top level scripts for that source.
const gSourceRoots = new ArrayMap();

gDebugger.onNewScript = (script) => {
  if (
    !isRecordingOrReplaying ||
    RecordReplayControl.areThreadEventsDisallowed()
  ) {
    return;
  }

  if (!considerScript(script)) {
    ignoreScript(script);
    return;
  }

  addScript(script);

  gSourceRoots.add(script.source, script);

  registerSource(script.source);

  function addScript(script) {
    const id = gScripts.add(script);
    script.setInstrumentationId(id);
    script.getChildScripts().forEach(addScript);
  }

  function ignoreScript(script) {
    script.setInstrumentationId(0);
    script.getChildScripts().forEach(ignoreScript);
  }
};

// All domains within which scripts were loaded.
const gScriptDomains = new Set();

function registerSource(source) {
  if (gSources.getId(source)) {
    return;
  }
  gSources.add(source);

  const id = sourceToProtocolSourceId(source);

  gGeckoSources.set(source.id, source);

  const window = getWindow();
  let sourceURL = getDebuggerSourceURL(source);
  RecordReplayControl.recordReplayAssert(`RegisterSource #1 ${sourceURL}`);

  if (!sourceURL && source.displayURL) {
    try {
      sourceURL = new URL(source.displayURL, window?.location?.href).toString();
    } catch {}
  }

  RecordReplayControl.recordReplayAssert(`RegisterSource #2 ${sourceURL}`);

  if (source.text !== "[wasm]") {
    setSourceMap({
      window,
      object: source,
      objectURL: sourceURL,
      objectText: source.text,
      objectMapURL: source.sourceMapURL,
    });
  }

  let kind = "scriptSource";
  if (source.introductionType === "inlineScript") {
    kind = "inlineScript";
  }

  try {
    const domain = new URL(sourceURL).hostname;
    gScriptDomains.add(domain);
  } catch (e) {}

  RecordReplayControl.onNewSource(id, kind, sourceURL);
}

const gHtmlContent = new Map();

function Target_getHTMLSource({ url }) {
  const info = gHtmlContent.get(url);
  const contents = info ? info.content : "";
  return { contents };
}

function isInterestingHTML(uri) {
  // Ignore URIs loaded into extension content processes.
  if (uri.startsWith("data:text/html") && uri.includes("moz-extension://")) {
    return false;
  }
  return true;
}

// We add the URI of the first loaded page as recording metadata.
let gHasHTMLContent = false;

function OnHTMLContent(data) {
  const { uri, contents } = JSON.parse(data);
  if (gHtmlContent.has(uri)) {
    gHtmlContent.get(uri).content += contents;
  } else {
    if (!gHasHTMLContent && isInterestingHTML(uri)) {
      gHasHTMLContent = true;
      RecordReplayControl.addMetadata({ uri });
    }
    gHtmlContent.set(uri, { content: contents, contentType: "text/html" });
  }
}

Services.obs.addObserver(
  {
    observe(_1, _2, data) {
      OnHTMLContent(data);
    },
  },
  "devtools-html-content"
);

// Listen for style sheet changes. This has to be set after creating the window.
getWindow().docShell.chromeEventHandler.addEventListener(
  "DOMWindowCreated",
  () => {
    const window = getWindow();
    window.document.styleSheetChangeEventsEnabled = true;
  },
  true
);

// Notify the UI process when we find a style sheet with a source map.
getWindow().docShell.chromeEventHandler.addEventListener(
  "StyleSheetApplicableStateChanged",
  ({ stylesheet }) => {
    setSourceMap({
      window: getStylesheetWindow(stylesheet),
      object: stylesheet,
      objectURL: stylesheet.href,
      objectText: undefined,
      objectMapURL: stylesheet.sourceMapURL,
    });
  },
  true
);

let gReactDevtoolsInitialized = false;

gNewGlobalHooks.push(dbgWindow => {
  if (!gReactDevtoolsInitialized) {
    gReactDevtoolsInitialized = true;
    initReactDevtools(dbgWindow, RecordReplayControl);
  }
});

// This logic is mostly copied from actors/style-sheet.js
function getStylesheetWindow(stylesheet) {
  while (stylesheet.parentStyleSheet) {
    stylesheet = stylesheet.parentStyleSheet;
  }

  if (stylesheet.ownerNode) {
    const document =
      stylesheet.ownerNode.nodetype === Node.DOCUMENT_NODE
        ? stylesheet.ownerNode
        : stylesheet.ownerNode.ownerDocument;
    if (document.defaultView) {
      return document.defaultView;
    }
  }
  return getWindow();
}

const { DebuggerNotificationObserver } = Cu.getGlobalForObject(
  require("resource://devtools/shared/Loader.jsm")
);
const gNotificationObserver = new DebuggerNotificationObserver();
gNotificationObserver.addListener(eventListener);
gNewGlobalHooks.push((global) => {
  try {
    gNotificationObserver.connect(global.unsafeDereference());
  } catch (e) {}
});

const {
  eventBreakpointForNotification,
} = require("devtools/server/actors/utils/event-breakpoints");

function eventListener(info) {
  const event = eventBreakpointForNotification(gDebugger, info);
  if (event && (info.phase == "pre" || info.phase == "post")) {
    RecordReplayControl.onEvent(event, info.phase == "pre");
  }
}

// Get information about operations performed by the recording which are
// potentially sensitive and can be shown to users before deciding to make
// the recording public.
function buildRecordingOperations() {
  const scriptDomains = [...gScriptDomains];
  let cookies, storage;

  for (const { kind, value } of RecordReplayControl.recordingOperations()) {
    switch (kind) {
      case "cookie":
        if (!cookies) {
          cookies = new Set();
        }
        cookies.add(value);
        break;
      case "localStorage":
        if (!storage) {
          storage = new Set();
        }
        storage.add(localStorageDomain(value));
        break;
      case "indexedDB":
        if (!storage) {
          storage = new Set();
        }
        storage.add(indexedDBDomain(value));
        break;
    }
  }

  return {
    scriptDomains,
    cookies: cookies ? [...cookies] : undefined,
    storage: storage ? [...storage] : undefined,
  };

  function localStorageDomain(value) {
    // Local storage values include the domain with its characters reversed,
    // for whatever reason, followed by ".:$protocol:$port"
    const idx = value.lastIndexOf(".");
    if (idx < 0) {
      return value;
    }
    return value.substring(0, idx).split("").reverse().join("");
  }

  function indexedDBDomain(value) {
    // Indexed DB values include the protocol as well.
    const idx = value.lastIndexOf("/");
    if (idx < 0) {
      return value;
    }
    return value.substring(idx + 1);
  }
}

function SendRecordingFinished(recordingId) {
  try {
    const document = getWindow().document;
    const data = {
      id: recordingId,
      url: document.URL,
      title: document.title,
      duration: new Date() - startTime,
      lastScreenData: getWindowAsImageData(getWindow()),
      lastScreenMimeType: "image/jpeg",
      operations: buildRecordingOperations(),
    };
    Services.cpmm.sendAsyncMessage("RecordingFinished", data);
  } catch (e) {
    // If we finish the recording while shutting down then we'll get
    // exceptions while sending messages to the UI process. Ignore these
    // exceptions.
  }
}

function SendRecordingUnusable(why) {
  Services.cpmm.sendAsyncMessage("RecordingUnusable", { why });
}

function SendRecordingUnsupported(why) {
  Services.cpmm.sendAsyncMessage("RecordingStarting");
  Services.cpmm.sendAsyncMessage("RecordingUnusable", { why });
}

function SendUnsupportedFeature(feature, issueNumber) {
  Services.cpmm.sendAsyncMessage("RecordingUnsupportedFeature", { feature, issueNumber });
}

function OnTestCommand(str) {
  const [_, cmd, arg] = /(.*?) (.*)/.exec(str);
  switch (cmd) {
    case "RecReplaySendAsyncMessage":
      Services.cpmm.sendAsyncMessage(arg);
      break;
    default:
      dump(`Unrecognized Test Command ${cmd}\n`);
      break;
  }
}

const commands = {
  "Pause.evaluateInFrame": Pause_evaluateInFrame,
  "Pause.evaluateInGlobal": Pause_evaluateInGlobal,
  "Pause.getAllFrames": Pause_getAllFrames,
  "Pause.getExceptionValue": Pause_getExceptionValue,
  "Pause.getObjectPreview": Pause_getObjectPreview,
  "Pause.getObjectProperty": Pause_getObjectProperty,
  "Pause.getScope": Pause_getScope,
  "Pause.getTopFrame": Pause_getTopFrame,
  "Debugger.getPossibleBreakpoints": Debugger_getPossibleBreakpoints,
  "Debugger.getPossibleBreakpointsForMultipleSources": Debugger_getPossibleBreakpointsForMultipleSources,
  "Debugger.getSourceContents": Debugger_getSourceContents,
  "CSS.getAppliedRules": CSS_getAppliedRules,
  "CSS.getComputedStyle": CSS_getComputedStyle,
  "DOM.getAllBoundingClientRects": DOM_getAllBoundingClientRects,
  "DOM.getBoundingClientRect": DOM_getBoundingClientRect,
  "DOM.getBoxModel": DOM_getBoxModel,
  "DOM.getDocument": DOM_getDocument,
  "DOM.getEventListeners": DOM_getEventListeners,
  "DOM.performSearch": DOM_performSearch,
  "DOM.querySelector": DOM_querySelector,
  "Graphics.getDevicePixelRatio": Graphics_getDevicePixelRatio,
  "Target.convertFunctionOffsetToLocation": Target_convertFunctionOffsetToLocation,
  "Target.convertFunctionOffsetsToLocations": Target_convertFunctionOffsetsToLocations,
  "Target.convertLocationToFunctionOffset": Target_convertLocationToFunctionOffset,
  "Target.countStackFrames": Target_countStackFrames,
  "Target.currentGeneratorId": Target_currentGeneratorId,
  "Target.getCurrentMessageContents": Target_getCurrentMessageContents,
  "Target.getFunctionsInRange": Target_getFunctionsInRange,
  "Target.getHTMLSource": Target_getHTMLSource,
  "Target.getStackFunctionIDs": Target_getStackFunctionIDs,
  "Target.getStepOffsets": Target_getStepOffsets,
  "Target.getSourceMapURL": Target_getSourceMapURL,
  "Target.getSheetSourceMapURL": Target_getSheetSourceMapURL,
  "Target.topFrameLocation": Target_topFrameLocation,
  "Target.getCurrentNetworkRequestEvent": Target_getCurrentNetworkRequestEvent,
  "Target.getCurrentNetworkStreamData": Target_getCurrentNetworkStreamData,
};

function OnProtocolCommand(method, params) {
  if (commands[method]) {
    try {
      return commands[method](params);
    } catch (e) {
      log(`Error: Exception processing command ${method}: ${e} ${e.stack}`);
      return null;
    }
  }
  log(`Error: Unsupported command ${method}`);
}

const exports = {
  SendRecordingFinished,
  SendRecordingUnusable,
  SendRecordingUnsupported,
  SendUnsupportedFeature,
  OnTestCommand,
  OnProtocolCommand,
  ClearPauseData,
  SetScanningScripts,
};

function Initialize() {
  return exports;
}

var EXPORTED_SYMBOLS = ["Initialize"];

///////////////////////////////////////////////////////////////////////////////
// Instrumentation
///////////////////////////////////////////////////////////////////////////////

gNewGlobalHooks.push((global) => {
  global.setInstrumentation(
    global.makeDebuggeeNativeFunction(
      RecordReplayControl.instrumentationCallback
    ),
    ["main", "entry", "breakpoint", "exit", "generator"]
  );

  if (RecordReplayControl.isScanningScripts()) {
    global.setInstrumentationActive(true);
  }
});

function SetScanningScripts(value) {
  gAllGlobals.forEach((g) => {
    try {
      g.setInstrumentationActive(value);
    } catch (e) {
      log(`Error: Exception setting instrumentation ${e}`);
    }
  });
}

///////////////////////////////////////////////////////////////////////////////
// Console Commands
///////////////////////////////////////////////////////////////////////////////

function geckoSourceIdToProtocolId(sourceId) {
  const source = gGeckoSources.get(sourceId);
  return source ? sourceToProtocolSourceId(source) : undefined;
}

let gCurrentConsoleMessage;

function OnConsoleError(message) {
  const target = message.timeWarpTarget || 0;

  let level = "error";
  if (message.flags & Ci.nsIScriptError.warningFlag) {
    level = "warning";
  } else if (message.flags & Ci.nsIScriptError.infoFlag) {
    level = "info";
  }

  // Diagnostics for TypeErrors that don't have an associated warp target.
  if (!target && String(message.errorMessage).includes("TypeError")) {
    log(
      `Error: TypeError message without a warp target "${message.errorMessage}" ${message.sourceId}`
    );
  }

  gCurrentConsoleMessage = {
    source: "PageError",
    level,
    text: message.errorMessage,
    url: message.sourceName,
    sourceId: geckoSourceIdToProtocolId(message.sourceId),
    line: message.lineNumber,
    column: message.columnNumber,
  };
  RecordReplayControl.onConsoleMessage(target);
  gCurrentConsoleMessage = null;
}

if (isRecordingOrReplaying) {
  Services.console.registerListener({
    observe(message) {
      if (message instanceof Ci.nsIScriptError) {
        OnConsoleError(message);
      }
    },
  });
}

function consoleAPIMessageLevel({ level }) {
  switch (level) {
    case "trace":
      return "trace";
    case "warn":
      return "warning";
    case "error":
      return "error";
    case "assert":
      return "assert";
    default:
      return "info";
  }
}

function OnConsoleAPICall(message) {
  message = message.wrappedJSObject;

  gCurrentConsoleMessage = {
    source: "ConsoleAPI",
    level: consoleAPIMessageLevel(message),
    text: "",
    url: message.filename,
    sourceId: geckoSourceIdToProtocolId(message.sourceId),
    line: message.lineNumber,
    column: message.columnNumber,
    messageArguments: message.arguments,
  };
  RecordReplayControl.onConsoleMessage(0);
  gCurrentConsoleMessage = null;
}

if (isRecordingOrReplaying) {
  Services.obs.addObserver(
    {
      observe(message) {
        OnConsoleAPICall(message);
      },
    },
    "console-api-log-event"
  );
}

function Target_getCurrentMessageContents() {
  assert(gCurrentConsoleMessage);

  // We need to create protocol values for the raw arguments within this command,
  // as the paused objects might have been cleared out after after notifying
  // the driver.
  let argumentValues;
  if (gCurrentConsoleMessage.messageArguments) {
    argumentValues = gCurrentConsoleMessage.messageArguments.map(createProtocolValueRaw);
  }

  return {
    ...gCurrentConsoleMessage,
    argumentValues,
    messageArguments: undefined,
  };
}

///////////////////////////////////////////////////////////////////////////////
// Debugger Commands
///////////////////////////////////////////////////////////////////////////////

function positionPrecedes(posA, posB) {
  return (
    posA.line < posB.line ||
    (posA.line == posB.line && posA.column < posB.column)
  );
}

// Whether line/column are in the range described by begin/end.
function positionMatches(begin, end, line, column) {
  if (begin && positionPrecedes({ line, column }, begin)) {
    return false;
  }
  if (end && positionPrecedes(end, { line, column })) {
    return false;
  }
  return true;
}

// Invoke callback on an overapproximation of all scripts in a source
// between begin and end.
function forMatchingScripts(source, begin, end, callback) {
  const roots = gSourceRoots.map.get(source);
  if (roots) {
    processScripts(roots);
  }

  // Whether script overaps with the selected range.
  function scriptMatches(script) {
    let lineCount;
    try {
      lineCount = script.lineCount;
    } catch (e) {
      // Watch for optimized out scripts.
      return false;
    }

    if (end) {
      const startPos = { line: script.startLine, column: script.startColumn };
      if (positionPrecedes(end, startPos)) {
        return false;
      }
    }

    if (begin) {
      const endPos = {
        line: script.startLine + lineCount - 1,

        // There is no endColumn accessor, so we can only compute this accurately
        // if the script is on a single line.
        column: lineCount == 1 ? script.startColumn + script.sourceLength : 1e9,
      };
      if (positionPrecedes(endPos, begin)) {
        return false;
      }
    }

    return true;
  }

  function processScripts(scripts) {
    for (const script of scripts) {
      if (scriptMatches(script)) {
        callback(script);
        processScripts(script.getChildScripts());
      }
    }
  }
}

// Invoke callback all positions in a source between begin and end (inclusive / optional).
function forMatchingBreakpointPositions(source, begin, end, callback) {
  forMatchingScripts(source, begin, end, (script) => {
    script
      .getPossibleBreakpoints()
      .forEach(({ offset, lineNumber, columnNumber }, i) => {
        if (positionMatches(begin, end, lineNumber, columnNumber)) {
          callback(script, offset, lineNumber, columnNumber);
        } else if (
          i == 0 &&
          positionMatches(begin, end, script.startLine, script.startColumn)
        ) {
          // The start location of the script is considered to match the first
          // breakpoint position. This allows setting breakpoints or analyses by
          // using the function location provided in the protocol, instead of
          // requiring the client to find the exact breakpoint position.
          callback(script, offset, lineNumber, columnNumber);
        }
      });
  });
}

function protocolSourceIdToSource(sourceId) {
  const source = gSources.getObject(Number(sourceId));
  assert(source, "source is unknown: " + sourceId);
  return source;
}

function sourceToProtocolSourceId(source) {
  const id = gSources.getId(source);
  assert(id > 0, "source is unknown");
  return String(id);
}

function Debugger_getPossibleBreakpoints({ sourceId, begin, end }) {
  const source = protocolSourceIdToSource(sourceId);

  const lineLocations = new ArrayMap();
  forMatchingBreakpointPositions(
    source,
    begin,
    end,
    (script, offset, line, column) => {
      lineLocations.add(line, column);
    }
  );

  return { lineLocations: finishLineLocations(lineLocations) };

  // Convert a line => columns ArrayMap into a lineLocations WRP object.
  function finishLineLocations(lineLocations) {
    return [...lineLocations.map.entries()].map(([line, columns]) => {
      return { line, columns };
    });
  }
}

function Debugger_getPossibleBreakpointsForMultipleSources({ sourceIds }) {
  return sourceIds.map((sourceId => {
    return {
      sourceId,
      ...Debugger_getPossibleBreakpoints({sourceId})
    }
  }));
}

function functionIdToScript(functionId) {
  const script = gScripts.getObject(Number(functionId));
  assert(script, "script is unknown: " + functionId);
  return script;
}

function scriptToFunctionId(script) {
  const id = gScripts.getId(script);
  assert(id > 0, "script is unknown");
  return String(id);
}

function Target_convertFunctionOffsetToLocation({ functionId, offset }) {
  const script = functionIdToScript(functionId);
  const sourceId = sourceToProtocolSourceId(script.source);

  if (offset === undefined) {
    const location = {
      sourceId,
      line: script.startLine,
      column: script.startColumn,
    };
    return { location };
  }

  const meta = script.getOffsetMetadata(offset);
  if (!meta) {
    throw new Error(`convertFunctionOffsetToLocation unknown offset ${offset}`);
  }
  if (!meta.isBreakpoint) {
    throw new Error(`convertFunctionOffsetToLocation non-breakpoint offset ${offset}`);
  }
  const location = { sourceId, line: meta.lineNumber, column: meta.columnNumber };
  return { location };
}

function Target_convertFunctionOffsetsToLocations({ functionId, offsets }) {
  const script = functionIdToScript(functionId);
  const sourceId = sourceToProtocolSourceId(script.source);

  const metaArray = script.getOffsetMetadataArray(offsets);
  const locations = [];
  for (const meta of metaArray) {
    if (!meta) {
      throw new Error(`convertFunctionOffsetToLocation unknown offset ${offset}`);
    }
    if (!meta.isBreakpoint) {
      throw new Error(`convertFunctionOffsetToLocation non-breakpoint offset ${offset}`);
    }
    locations.push({ sourceId, line: meta.lineNumber, column: meta.columnNumber });
  }
  return { locations };
}

function Target_convertLocationToFunctionOffset({ location }) {
  const { sourceId, line, column } = location;
  const source = protocolSourceIdToSource(sourceId);
  const target = { line, column };

  let rv = {};
  forMatchingBreakpointPositions(source, target, target, (script, offset) => {
    rv = { functionId: scriptToFunctionId(script), offset };
  });
  return rv;
}

function Target_getFunctionsInRange({ sourceId, begin, end }) {
  const source = protocolSourceIdToSource(sourceId);

  const functions = [];
  forMatchingScripts(source, begin, end, (script) => {
    functions.push(scriptToFunctionId(script));
  });
  return { functions };
}

function Target_getStackFunctionIDs() {
  const rv = [];
  forEachScriptFrame(frame => {
    rv.push(scriptToFunctionId(frame.script));
  });
  return { frameFunctions: rv.reverse() };
}

function Target_getStepOffsets({ functionId }) {
  const script = functionIdToScript(functionId);
  const offsets = script
    .getPossibleBreakpoints()
    .filter((bp) => bp.isStepStart)
    .map((bp) => bp.offset);
  return { offsets };
}

function Target_getSourceMapURL({ sourceId }) {
  const source = protocolSourceIdToSource(sourceId);
  return getSourceMapData(source) || {};
}

function Debugger_getSourceContents({ sourceId }) {
  const source = protocolSourceIdToSource(sourceId);

  let contents = source.text;
  if (source.startLine > 1) {
    contents = "\n".repeat(source.startLine - 1) + contents;
  }

  return {
    contents,
    contentType: "text/javascript",
  };
}

///////////////////////////////////////////////////////////////////////////////
// Network Commands
///////////////////////////////////////////////////////////////////////////////

/**
 * Networking in Gecko is complex because requests themselves are handled in the
 * parent process, even though the page itself is executed and rendered in the
 * child process.
 *
 * Primary notes:
 *  * Only a few http-on-* events are available in the child, so the parent
 *    is the only place to observe basic response data consistently.
 *  * Only the parent process has an API for observing request timing and
 *    capturing raw request header data.
 *  * Observing request-start in the child process is important to allow us to
 *    capture execution info so we can jump to that point in the recording later.
 *  * Observing request-done in the child process is important because decoding
 *    of Content-Encoding request data happens in the child process, so the
 *    parent process has no access to 'decodedBodySize'.
 *
 * To handle all of these cases consistently, we watch for the minimal number of
 * parent-process events that we need, and send IPC messages to the child so
 * that the child can directly use that data.
 */

let gCurrentRequestEvent;

if (isRecordingOrReplaying) {
  // ChannelId => BookmarkId
  // While a request is in-progress, this map will have an entry for the channelId
  // and will track the bookmark that was used for the request, so if the request
  // is redirected and a new channel forks off of this one, its bookmark can be
  // applied to the new channel as well.
  const gActiveRequests = new Map();

  // ChannelId => BookmarkId
  // When a channel redirects, we need to save the bookmark for the channel so that
  // it can be applied to the new channel created to handle the redirect.
  const gPendingRedirects = new Map();

  function getChannel(subject) {
    if (!(subject instanceof Ci.nsIHttpChannel)) {
      return null;
    }
    const channel = subject.QueryInterface(Ci.nsIHttpChannel);
    channel.QueryInterface(Ci.nsIHttpChannelInternal);

    return channel;
  }

  function onRequestStreamTopic(subject, topic, data) {
    const inputStream = subject.QueryInterface(Ci.nsIInputStream);
    const channelId = +data;
    const isRequestBody = topic === "replay-request-start";

    const streamId = `${isRequestBody ? "request" : "response"}-${channelId}`;
    notifyRequestEvent(channelId, isRequestBody ? "request-body" : "response-body", {});
    notifyNetworkStreamStart(streamId, isRequestBody ? "request-data" : "response-data", `${channelId}`);

    let offset = 0;
    listenForStreamData();

    function onStreamData(value) {
      notifyNetworkStreamData(streamId, offset, value.byteLength, ({ index, length }) => ({
        kind: "data",
        value: ChromeUtils.base64URLEncode(
          value.slice(index, index + length),
          { pad: true }
        ).replace(/-/g, "+").replace(/_/g, "/"),
      }));

      offset += value.byteLength;
    }

    function onStreamEnd() {
      notifyNetworkStreamEnd(streamId, offset);
    }

    function listenForStreamData() {
      inputStream.asyncWait({ onInputStreamReady }, 0, 0, Services.tm.mainThread);
    }

    function onInputStreamReady(stream) {
      assert(stream === inputStream);

      let available;
      try {
        available = inputStream.available();
      } catch {
        onStreamEnd();
        return;
      }

      if (available > 0) {
        const value = NetUtil.readInputStream(inputStream, available);
        onStreamData(value);
      }

      // For some reason, request streams don't appear to be marked closed properly.
      // Firefox doesn't support using an actual ReadableStream for fetch bodies,
      // so every request body SHOULD have all of its data available immediately,
      // meaning that we can immediately consider all request body streams to
      // end once they have been read.
      if (isRequestBody) {
        onStreamEnd();
        stream.close();
        return;
      }

      listenForStreamData();
    }
  }
  Services.obs.addObserver(onRequestStreamTopic, "replay-request-start");
  Services.obs.addObserver(onRequestStreamTopic, "replay-response-start");

  Services.obs.addObserver((subject, topic, data) => {
    const channel = getChannel(subject);
    if (!channel) {
      return;
    }

    onHttpOpeningRequest(channel.channelId, getChannelRequestData(channel), false);
  }, "http-on-opening-request");

  Services.cpmm.addMessageListener("RecordingChannelOpeningRequest", {
    receiveMessage(msg) {
      const { channelId, data } = msg.data;
      onHttpOpeningRequest(channelId, data, true);
    },
  });
  function onHttpOpeningRequest(channelId, data, fromParent) {
    if (gActiveRequests.has(channelId)) {
      return;
    }

    let bookmark;
    if (gPendingRedirects.has(channelId)) {
      // Some channels may be created as a result of other channels being redirected.
      // When that happens, we still want to treat the redirected request as if it
      // was initiated with the same callstack as the original request, so we have
      // to pull in the bookmark from the previous request if we can.
      bookmark = gPendingRedirects.get(channelId);
      gPendingRedirects.delete(channelId);
    } else if (fromParent) {
      // Requests opened by the parent process are never going to
      // have a pause location associated with them.
      bookmark = 0;
    } else {
      // Record the actual callstack location of this call so we can jump back to it.
      bookmark = RecordReplayControl.makeBookmark();
    }

    gActiveRequests.set(channelId, bookmark);

    RecordReplayControl.onHttpRequest(channelId.toString(), bookmark);

    notifyRequestEvent(channelId, "request", data);
  }

  Services.obs.addObserver((subject, topic, data) => {
    const channel = getChannel(subject);
    if (!channel) {
      return;
    }

    onHttpOpeningRequest(channel.channelId, getChannelRequestData(channel), false);
    onHttpFailedOpeningRequest(channel.channelId, getChannelRequestFailedData(channel));
  }, "http-on-failed-opening-request");
  Services.cpmm.addMessageListener("RecordingChannelRequestFailed", {
    receiveMessage(msg) {
      const { channelId, data } = msg.data;
      onHttpFailedOpeningRequest(channelId, data);
    },
  });
  function onHttpFailedOpeningRequest(channelId, data) {
    notifyRequestEvent(channelId, "request-failed", data);
  }

  Services.obs.addObserver((subject, topic, data) => {
    const channel = getChannel(subject);
    if (!channel) {
      return;
    }

    onHttpStopRequest(channel);
  }, "http-on-stop-request");

  function onHttpStopRequest(channel) {
    if (!gActiveRequests.has(channel.channelId)) {
      return;
    }

    gActiveRequests.delete(channel.channelId);

    notifyRequestEvent(channel.channelId, "request-done", getChannelRequestDoneData(channel));
  }

  function notifyRequestEvent(channelId, kind, data) {
    gCurrentRequestEvent = {
      kind,
      data,
    };
    RecordReplayControl.onHttpRequestEvent(channelId.toString());
    gCurrentRequestEvent = null;
  }

  Services.cpmm.addMessageListener("RecordingChannelRequestRawHeaders", {
    receiveMessage(msg) {
      const { channelId, requestRawHeaders } = msg.data;

      if (!gActiveRequests.has(channelId)) {
        return;
      }

      notifyRequestEvent(channelId, "request-raw-headers", {
        requestRawHeaders
      });
    },
  });

  Services.cpmm.addMessageListener("RecordingChannelResponseStart", {
    receiveMessage(msg) {
      const { channelId, data } = msg.data;

      if (!gActiveRequests.has(channelId)) {
        return;
      }

      const { remoteDestination, ...response } = data;

      if (remoteDestination) {
        notifyRequestEvent(channelId, "request-destination", {
          destinationAddress: remoteDestination.address,
          destinationPort: remoteDestination.port,
        });
      }
      notifyRequestEvent(channelId, "response", response);
    },
  });

  Services.cpmm.addMessageListener("RecordingChannelResponseRawHeaders", {
    receiveMessage(msg) {
      const { channelId, responseRawHeaders } = msg.data;

      if (!gActiveRequests.has(channelId)) {
        return;
      }

      notifyRequestEvent(channelId, "response-raw-headers", {
        responseRawHeaders,
      });
    },
  });

  const SINK_CLASS_DESCRIPTION = "Replay Networking Event Sink";
  const SINK_CLASS_ID = Components.ID("{96692fd5-4d0d-4656-9283-fdf0e3c65553}");
  const SINK_CONTRACT_ID = "@mozilla.org/network/monitor/channeleventsink;1";
  const SINK_CATEGORY_NAME = "net-channel-event-sinks";
  class ReplayChannelEventSink {
    asyncOnChannelRedirect(oldChannel, newChannel, flags, callback) {
      const oldChan = getChannel(oldChannel);
      const newChan = getChannel(newChannel);

      const bookmark = oldChan ? gActiveRequests.get(oldChan.channelId) : null;
      if (newChan && typeof bookmark === "number") {
        gPendingRedirects.set(newChan.channelId, bookmark);

        // It seems that redirected requests don't fire the done event.
        onHttpStopRequest(oldChan);
      }

      callback.onRedirectVerifyCallback(Cr.NS_OK);
    }
  }
  ReplayChannelEventSink.prototype.QueryInterface = ChromeUtils.generateQI([
    "nsIChannelEventSink",
  ]);
  const ReplayChannelEventSinkFactory = ComponentUtils.generateSingletonFactory(
    ReplayChannelEventSink
  );
  const registrar = Components.manager.QueryInterface(Ci.nsIComponentRegistrar);
  registrar.registerFactory(
    SINK_CLASS_ID,
    SINK_CLASS_DESCRIPTION,
    SINK_CONTRACT_ID,
    ReplayChannelEventSinkFactory
  );
  Services.catMan.addCategoryEntry(
    SINK_CATEGORY_NAME,
    SINK_CONTRACT_ID,
    SINK_CONTRACT_ID,
    false,
    true
  );

  function notifyNetworkStreamStart(id, kind, parentId) {
    RecordReplayControl.onNetworkStreamStart(id, kind, parentId);
  }
  function notifyNetworkStreamData(id, offset, length, callback) {
    gCurrentNetworkStreamDataCallback = callback;
    RecordReplayControl.onNetworkStreamData(id, offset, length);
    gCurrentNetworkStreamDataCallback = null;
  }
  function notifyNetworkStreamEnd(id, length) {
    RecordReplayControl.onNetworkStreamEnd(id, length);
  }
}


///////////////////////////////////////////////////////////////////////////////
// Graphics Commands
///////////////////////////////////////////////////////////////////////////////

function Graphics_getDevicePixelRatio() {
  return { ratio: getWindow().devicePixelRatio };
}

///////////////////////////////////////////////////////////////////////////////
// Pause Commands
///////////////////////////////////////////////////////////////////////////////

// Associate Debugger.{Object,Env} with the number to use for the protocol ID.
const gPauseObjects = new IdMap();

// Map raw object => Debugger.Object
const gCanonicalObjects = new Map();

// Clear out object state and associated strong references.
function ClearPauseData() {
  gPauseObjects.clear();
  gCanonicalObjects.clear();
}

function getObjectId(obj) {
  if (!obj) {
    return "0";
  }
  assert(obj instanceof Debugger.Object || obj instanceof Debugger.Environment);

  let id = gPauseObjects.getId(obj);
  if (id) {
    return String(id);
  }

  // Sometimes there are multiple Debugger.Objects for the same underlying
  // object. Make sure a consistent ID is used.
  if (obj instanceof Debugger.Object) {
    const raw = obj.unsafeDereference();
    if (gCanonicalObjects.has(raw)) {
      const canonical = gCanonicalObjects.get(raw);
      return String(gPauseObjects.getId(canonical));
    }
    gCanonicalObjects.set(raw, obj);
  }

  return String(gPauseObjects.add(obj));
}

function getObjectFromId(id) {
  return gPauseObjects.getObject(Number(id));
}

function makeDebuggeeValue(value) {
  if (!isNonNullObject(value)) {
    return value;
  }
  assert(!(value instanceof Debugger.Object));
  try {
    const dbgGlobal = gDebugger.makeGlobalObjectReference(
      Cu.getGlobalForObject(value)
    );
    return dbgGlobal.makeDebuggeeValue(value);
  } catch (e) {
    return gSandboxGlobal.makeDebuggeeValue(value);
  }
}

function getObjectIdRaw(obj) {
  return getObjectId(makeDebuggeeValue(obj));
}

const UnserializablePrimitives = [
  [Infinity, "Infinity"],
  [-Infinity, "-Infinity"],
  [NaN, "NaN"],
  [-0, "-0"],
];

function maybeUnserializableNumber(v) {
  for (const [unserializable, str] of UnserializablePrimitives) {
    if (Object.is(v, unserializable)) {
      return str;
    }
  }
}

// Strings longer than this will be truncated when creating protocol values.
const MaxStringLength = 10000;

function createProtocolValue(v) {
  if (isNonNullObject(v)) {
    if (v.optimizedOut || v.missingArguments) {
      return { unavailable: true };
    }
    if (v.uninitialized) {
      return { uninitialized: true };
    }
    return { object: getObjectId(v) };
  }
  if (v === undefined) {
    return {};
  }
  const unserializableNumber = maybeUnserializableNumber(v);
  if (unserializableNumber) {
    return { unserializableNumber };
  }
  if (typeof v == "bigint") {
    return { bigint: v.toString() };
  }
  if (typeof v == "string" && v.length > MaxStringLength) {
    return { value: v.substring(0, MaxStringLength) + "…" };
  }
  if (typeof v == "symbol") {
    return { symbol: v.toString() };
  }
  return { value: v };
}

function createProtocolValueRaw(v) {
  return createProtocolValue(makeDebuggeeValue(v));
}

function createProtocolPropertyDescriptor(name, desc) {
  const rv = createProtocolValue(desc.value);
  rv.name = name.toString();

  let flags = 0;
  if (desc.writable) {
    flags |= 1;
  }
  if (desc.configurable) {
    flags |= 2;
  }
  if (desc.enumerable) {
    flags |= 4;
  }
  if (flags != 7) {
    rv.flags = flags;
  }

  if (desc.get) {
    rv.get = getObjectId(desc.get);
  }
  if (desc.set) {
    rv.set = getObjectId(desc.set);
  }

  if (typeof name == "symbol") {
    rv.isSymbol = true;
  }

  return rv;
}

function getFunctionName(obj) {
  return obj.name || obj.displayName;
}

function getFunctionLocation(obj) {
  let { script } = obj;
  if (!script && obj.isBoundFunction) {
    script = obj.boundTargetFunction.script;
  }
  if (script && gScripts.getId(script) === 0) {
    // sourceToProtocolSourceId will throw for unknown sources, so if
    // something tries to get the location of an unknown script, we
    // should bail.
    script = null;
  }
  if (script) {
    return {
      sourceId: sourceToProtocolSourceId(script.source),
      line: script.startLine,
      column: script.startColumn,
    };
  }
}

function getFrameLocation(frame) {
  // Find the line/column for this frame. This is a bit tricky because we want
  // positions that are consistent with those for any breakpoint we are
  // paused at. When pausing at a breakpoint the frame won't actually be at
  // that breakpoint, it will be at an instrumentation opcode shortly before
  // the breakpoint, which might have a different position. So, we adjust the
  // frame offset by a fixed amount to get to the breakpoint, and if it isn't
  // a breakpoint then we are not at an instrumentation opcode in the frame
  // and can use the frame's normal offset.
  const CallBreakpointOffset = 9;
  let offset = frame.offset;
  try {
    if (
      frame.script.getOffsetMetadata(offset + CallBreakpointOffset).isBreakpoint
    ) {
      offset += CallBreakpointOffset;
    }
  } catch (e) {}
  const { lineNumber, columnNumber } = frame.script.getOffsetMetadata(offset);

  const sourceId = sourceToProtocolSourceId(frame.script.source);
  return {
    sourceId,
    line: lineNumber,
    column: columnNumber,
  };
}

function createProtocolFrame(frameId, frame) {
  const type = getFrameType(frame);

  let functionName;
  let functionLocation;
  if (frame.type == "call") {
    functionName = getFunctionName(frame.callee);
    const location = getFunctionLocation(frame.callee);
    assert(location, "unknown frame callee location");
    functionLocation = [location];
  }

  const scopeChain = getScopeChain(frame);
  const thisv = createProtocolValue(frame.this);

  return {
    frameId,
    type,
    functionName,
    functionLocation,
    location: [getFrameLocation(frame)],
    scopeChain,
    this: thisv,
  };

  // Get the protocol type to use for frame.
  function getFrameType(frame) {
    switch (frame.type) {
      case "call":
      case "wasmcall":
        return "call";
      case "eval":
      case "debugger":
        return "eval";
      case "global":
        return "global";
      case "module":
        return "module";
    }
    throw new Error(`Bad frame type ${frame.type}`);
  }

  function getScopeChain(frame) {
    const scopeChain = [];
    for (let env = frame.environment; env; env = env.parent) {
      const scopeId = getObjectId(env);
      scopeChain.push(scopeId);
    }
    return scopeChain;
  }
}

function getClassName(obj) {
  return obj.isProxy ? "Proxy" : obj.class;
}

function createProtocolObject(objectId, level) {
  const obj = getObjectFromId(objectId);

  if (!obj) {
    log("Error: createProtocolObject unknown object");
    return { objectId, className: "BadObjectId" };
  }

  const className = getClassName(obj)
  let preview;
  if (level != "none") {
    preview = new ProtocolObjectPreview(obj, level).fill();
  }

  return { objectId, className, preview };
}

// Return whether an object should be ignored when generating previews.
function isObjectBlacklisted(obj) {
  // Accessing Storage object properties can cause hangs when trying to
  // communicate with the non-existent parent process.
  if (obj.class == "Storage") {
    return true;
  }

  // Don't inspect scripted proxies, as we could end up calling into script.
  if (obj.isProxy) {
    return true;
  }

  return false;
}

// Return whether an object's property should be ignored when generating previews.
function isObjectPropertyBlacklisted(obj, name) {
  if (isObjectBlacklisted(obj)) {
    return true;
  }
  switch (`${obj.class}.${name}`) {
    case "Window.localStorage":
    case "Window.sysinfo":
    case "Navigator.hardwareConcurrency":
    case "XPCWrappedNative_NoHelper.isParentWindowMainWidgetVisible":
    case "XPCWrappedNative_NoHelper.systemFont":
      return true;
  }
  switch (name) {
    case "__proto__":
      // Accessing __proto__ doesn't cause problems, but is redundant with the
      // prototype reference included in the preview directly.
      return true;
  }
  return false;
}

// Get the "own" property names of an object to use.
function propertyNames(object) {
  if (isObjectBlacklisted(object)) {
    return [];
  }
  try {
    return [...object.getOwnPropertyNames(), ...object.getOwnPropertySymbols()];
  } catch (e) {
    return [];
  }
}

// Return whether a getter should be callable without having side effects.
function safeGetter(getter) {
  // For now we only allow calling native C++ getters.
  return getter.class == "Function" && !getter.script;
}

// Target limit for the number of items (properties etc.) to include in object
// previews before overflowing.
const MaxItems = {
  "noProperties": 0,
  "canOverflow": 10,
  "full": 1000,
};

// Structure for managing construction of protocol ObjectPreview objects.
function ProtocolObjectPreview(obj, level) {
  // Underlying Debugger.Object
  this.obj = obj;

  // Underlying JSObject
  this.raw = obj.unsafeDereference();

  // Additional properties to add to the resulting preview.
  this.extra = {};

  // ObjectPreviewLevel for the resulting preview. Not "none".
  this.level = level;

  // Whether the preview did overflow.
  this.overflow = false;

  // How many items have been added to the object, for determining when to overflow.
  this.numItems = 0;
}

ProtocolObjectPreview.prototype = {
  canAddItem(force) {
    if (!force && this.numItems >= MaxItems[this.level]) {
      this.overflow = true;
      return false;
    }
    this.numItems++;
    return true;
  },

  addProperty(property) {
    if (!this.canAddItem()) {
      return;
    }
    if (!this.properties) {
      this.properties = [];
    }
    this.properties.push(property);
  },

  addGetterValue(name, force) {
    if (isObjectPropertyBlacklisted(this.obj, name)) {
      return;
    }
    if (!this.getterValues) {
      this.getterValues = new Map();
    }
    if (this.getterValues.has(name)) {
      return;
    }
    if (!this.canAddItem(force)) {
      return;
    }
    try {
      const value = createProtocolValueRaw(this.raw[name]);
      this.getterValues.set(name, { name, ...value });
    } catch (e) {
      this.numItems--;
    }
  },

  addContainerEntry(entry) {
    if (!this.canAddItem()) {
      return;
    }
    if (!this.containerEntries) {
      this.containerEntries = [];
    }
    this.containerEntries.push(entry);
  },

  addContainerEntryRaw(value, key, hasKey) {
    key = hasKey ? createProtocolValueRaw(key) : undefined;
    value = createProtocolValueRaw(value);
    this.addContainerEntry({ key, value });
  },

  fill() {
    let prototypeId;
    if (this.obj.proto && !this.obj.isProxy) {
      try {
        prototypeId = getObjectId(this.obj.proto);
      } catch (e) {}
    }

    // Add class-specific data.
    const className = getClassName(this.obj);
    const previewer = CustomPreviewers[className];
    if (previewer) {
      for (const entry of previewer) {
        if (typeof entry == "string") {
          // Getters in class specific data are always added, even if they
          // would otherwise cause the preview to overflow.
          this.addGetterValue(entry, /* force */ true);
        } else {
          entry.call(this);
        }
      }
    }

    if (this.level != "noProperties") {
      // Add "own" properties of the object.
      const names = propertyNames(this.obj);
      for (const name of names) {
        try {
          const desc = this.obj.getOwnPropertyDescriptor(name);
          const property = createProtocolPropertyDescriptor(name, desc);
          this.addProperty(property);
        } catch (e) {}
        if (this.overflow) {
          break;
        }
      }

      // Add values of getters found on the prototype chain.
      this.addPrototypeGetterValues();
    } else {
      this.overflow = true;
    }

    // Add data for DOM/CSS objects.
    if (Node.isInstance(this.raw)) {
      this.extra.node = nodeContents(this.raw);
    } else if (CSSRule.isInstance(this.raw)) {
      this.extra.rule = ruleContents(this.raw);
    } else if (CSSStyleDeclaration.isInstance(this.raw)) {
      this.extra.style = styleContents(this.raw);
    } else if (StyleSheet.isInstance(this.raw)) {
      this.extra.styleSheet = styleSheetContents(this.raw);
    }

    let getterValues;
    if (this.getterValues) {
      getterValues = [...this.getterValues.values()];
    }

    return {
      prototypeId,
      overflow: (this.overflow && this.level != "full") ? true : undefined,
      properties: this.properties,
      containerEntries: this.containerEntries,
      getterValues,
      ...this.extra,
    };
  },

  // Add any getter values on the preview for properties found on the prototype chain.
  addPrototypeGetterValues() {
    const seen = new Set();
    let proto = this.obj;
    while (proto) {
      for (const name of propertyNames(proto)) {
        if (seen.has(name)) {
          continue;
        }
        seen.add(name);
        try {
          const desc = proto.getOwnPropertyDescriptor(name);
          if (desc.get && safeGetter(desc.get)) {
            this.addGetterValue(name);
            if (this.overflow) {
              return;
            }
          }
        } catch (e) {}
      }
      try {
        proto = proto.proto;
      } catch (e) {
        break;
      }
    }
  },
};

function previewMap() {
  const entries = Cu.waiveXrays(Map.prototype.entries.call(this.raw));
  for (const [k, v] of entries) {
    this.addContainerEntryRaw(v, k, true);
    if (this.overflow) {
      break;
    }
  }

  this.extra.containerEntryCount = this.raw.size;
}

function previewWeakMap() {
  const keys = ChromeUtils.nondeterministicGetWeakMapKeys(this.raw);
  this.extra.containerEntryCount = keys.length;

  for (const k of keys) {
    const v = WeakMap.prototype.get.call(this.raw, k);
    this.addContainerEntryRaw(v, k, true);
    if (this.overflow) {
      break;
    }
  }
}

function previewSet() {
  const values = Cu.waiveXrays(Set.prototype.values.call(this.raw));
  for (const v of values) {
    this.addContainerEntryRaw(v);
    if (this.overflow) {
      break;
    }
  }

  this.extra.containerEntryCount = this.raw.size;
}

function previewWeakSet() {
  const keys = ChromeUtils.nondeterministicGetWeakSetKeys(this.raw);
  this.extra.containerEntryCount = keys.length;

  for (const k of keys) {
    this.addContainerEntryRaw(k);
    if (this.overflow) {
      break;
    }
  }
}

function previewPromise() {
  let state;
  let value;

  switch (this.obj.promiseState) {
    case "fulfilled":
      state = "fulfilled";
      value = createProtocolValue(this.obj.promiseValue);
      break;
    case "rejected":
      state = "rejected";
      value = createProtocolValue(this.obj.promiseReason);
      break;
    default:
      state = "pending";
      break;
  }

  this.extra.promiseState = { state, value };
}

function previewProxy() {
  this.extra.proxyState = {
    target: createProtocolValue(this.obj.proxyTarget),
    handler: createProtocolValue(this.obj.proxyHandler),
  };
}

function previewRegExp() {
  this.extra.regexpString = this.raw.toString();
}

function previewDate() {
  this.extra.dateTime = this.raw.getTime();
}

const ErrorProperties = [
  "name",
  "message",
  "stack",
  "fileName",
  "lineNumber",
  "columnNumber",
];

function previewFunction() {
  this.extra.functionName = getFunctionName(this.obj);

  const functionLocation = getFunctionLocation(this.obj);
  if (functionLocation) {
    this.extra.functionLocation = [functionLocation];
  }

  const parameterNames = (this.obj.parameterNames || []).filter(
    (n) => typeof n == "string"
  );
  this.extra.functionParameterNames = parameterNames;
}

const CustomPreviewers = {
  Array: ["length"],
  Int8Array: ["length"],
  Uint8Array: ["length"],
  Uint8ClampedArray: ["length"],
  Int16Array: ["length"],
  Uint16Array: ["length"],
  Int32Array: ["length"],
  Uint32Array: ["length"],
  Float32Array: ["length"],
  Float64Array: ["length"],
  BigInt64Array: ["length"],
  BigUint64Array: ["length"],
  Map: ["size", previewMap],
  WeakMap: [previewWeakMap],
  Set: ["size", previewSet],
  WeakSet: [previewWeakSet],
  Promise: [previewPromise],
  Proxy: [previewProxy],
  RegExp: ["global", "source", previewRegExp],
  Date: [previewDate],
  Error: ErrorProperties,
  EvalError: ErrorProperties,
  RangeError: ErrorProperties,
  ReferenceError: ErrorProperties,
  SyntaxError: ErrorProperties,
  TypeError: ErrorProperties,
  URIError: ErrorProperties,
  Function: [previewFunction],
  MouseEvent: ["type", "target", "clientX", "clientY", "layerX", "layerY"],
  KeyboardEvent: [
    "type",
    "target",
    "key",
    "charCode",
    "keyCode",
    "altKey",
    "ctrlKey",
    "metaKey",
    "shiftKey",
  ],
  MessageEvent: ["type", "target", "isTrusted", "data"],
};

function getPseudoType(node) {
  switch (node.localName) {
    case "_moz_generated_content_marker":
      return "marker";
    case "_moz_generated_content_before":
      return "before";
    case "_moz_generated_content_after":
      return "after";
  }
}

function nodeContents(node) {
  let attributes, pseudoType;
  if (Element.isInstance(node)) {
    attributes = [];
    for (const { name, value } of node.attributes) {
      attributes.push({ name, value });
    }
    pseudoType = getPseudoType(node);
  }

  let style;
  if (node.style) {
    style = getObjectIdRaw(node.style);
  }

  let parentNode;
  if (node.parentNode) {
    parentNode = getObjectIdRaw(node.parentNode);
  } else if (node.defaultView && node.defaultView.parent != node.defaultView) {
    // Nested documents use the parent element instead of null.
    const iframes = node.defaultView.parent.document.getElementsByTagName(
      "iframe"
    );
    const iframe = [...iframes].find((f) => f.contentDocument == node);
    if (iframe) {
      parentNode = getObjectIdRaw(iframe);
    }
  }

  let childNodes;
  if (node.childNodes.length) {
    childNodes = [...node.childNodes].map((n) => getObjectIdRaw(n));
  } else if (node.nodeName == "IFRAME") {
    // Treat an iframe's content document as one of its child nodes.
    childNodes = [getObjectIdRaw(node.contentDocument)];
  }

  let documentURL;
  if (node.nodeType == Node.DOCUMENT_NODE) {
    documentURL = node.URL;
  }

  return {
    nodeType: node.nodeType,
    nodeName: node.nodeName,
    nodeValue: typeof node.nodeValue === "string" ? node.nodeValue : undefined,
    isConnected: node.isConnected,
    attributes,
    pseudoType,
    style,
    parentNode,
    childNodes,
    documentURL,
  };
}

function ruleContents(rule) {
  let parentStyleSheet;
  if (rule.parentStyleSheet) {
    parentStyleSheet = getObjectIdRaw(rule.parentStyleSheet);
  }

  let style;
  if (rule.style) {
    style = getObjectIdRaw(rule.style);
  }

  return {
    type: rule.type,
    cssText: rule.cssText,
    parentStyleSheet,
    startLine: InspectorUtils.getRelativeRuleLine(rule),
    startColumn: InspectorUtils.getRuleColumn(rule),
    selectorText: rule.selectorText,
    style,
  };
}

function styleContents(style) {
  let parentRule;
  if (style.parentRule) {
    parentRule = getObjectIdRaw(style.parentRule);
  }

  const properties = [];
  for (let i = 0; i < style.length; i++) {
    const name = style.item(i);
    const value = style.getPropertyValue(name);
    const important =
      style.getPropertyPriority(name) == "important" ? true : undefined;
    properties.push({ name, value, important });
  }

  return {
    cssText: style.cssText,
    parentRule,
    properties,
  };
}

function createProtocolScope(scopeId) {
  const env = getObjectFromId(scopeId);

  const type = getEnvType(env);
  const functionLexical = env.scopeKind == "function lexical";

  let object, bindings;
  if (env.type == "declarative") {
    bindings = [];
    for (const name of env.names()) {
      const v = env.getVariable(name);
      bindings.push({ name, ...createProtocolValue(v) });
    }
  } else {
    object = getObjectId(env.object);
  }

  let functionName;
  if (env.callee) {
    functionName = getFunctionName(env.callee);
  }

  return {
    scopeId,
    type,
    functionLexical,
    object,
    functionName,
    bindings,
  };

  // Get the prototocl type to use for env.
  function getEnvType(env) {
    switch (env.type) {
      case "object":
        return "global";
      case "with":
        return "with";
      case "declarative":
        return env.callee ? "function" : "block";
    }
    throw new Error(`Bad environment type ${env.type}`);
  }
}

function styleSheetContents(styleSheet) {
  return {
    href: styleSheet.href || undefined,
    isSystem: styleSheet.parsingMode != "author",
  };
}

function completionToProtocolResult(completion) {
  let returned, exception;
  if ("return" in completion) {
    returned = createProtocolValue(completion.return);
  }
  if ("throw" in completion) {
    exception = createProtocolValue(completion.throw);
  }
  return { returned, exception, data: {} };
}

function Pause_evaluateInFrame({ frameId, expression }) {
  const frameIndexNum = Number(frameId);
  const frame = findScriptFrame((_, i) => i === frameIndexNum);
  if (!frame) {
    throw new Error("Can't find frame");
  }

  const completion = frame.eval(expression);
  return { result: completionToProtocolResult(completion) };
}

function Pause_evaluateInGlobal({ expression }) {
  const dbgWindow = gDebugger.makeGlobalObjectReference(getWindow());
  const completion = dbgWindow.executeInGlobal(expression);
  return { result: completionToProtocolResult(completion) };
}

function Pause_getAllFrames() {
  const frameIds = [];
  const frameData = [];

  forEachScriptFrame((frame, i) => {
    const id = String(i);
    frameIds.push(id);
    frameData.push(createProtocolFrame(id, frame));
  });

  return {
    frames: frameIds.reverse(),
    data: { frames: frameData },
  };
}

function Pause_getTopFrame() {
  const numFrames = countScriptFrames();
  if (numFrames) {
    const frame = scriptFrameForIndex(numFrames - 1);
    const id = String(numFrames - 1);
    const frameData = createProtocolFrame(id, frame);
    return { frame: id, data: { frames: [frameData] } };
  }
  return { data: {} };
}

let gCurrentNetworkStreamDataCallback;

function Target_getCurrentNetworkStreamData(params) {
  assert(gCurrentNetworkStreamDataCallback, "must have network stream data");
  return {
    data: gCurrentNetworkStreamDataCallback(params),
  };
}

function Target_getCurrentNetworkRequestEvent() {
  assert(gCurrentRequestEvent);
  const { kind, data } = gCurrentRequestEvent;
  return { data: { kind, ...data } };
}

function Target_countStackFrames() {
  return { count: countScriptFrames() };
}

function Target_currentGeneratorId() {
  const { generatorId } = gDebugger.getNewestFrame();
  return { id: generatorId };
}

function Pause_getExceptionValue() {
  assert(gExceptionValue);
  return {
    exception: createProtocolValue(gExceptionValue.value),
    data: {},
  };
}

function Pause_getObjectPreview({ object, level = "full" }) {
  const objectData = createProtocolObject(object, level);
  return { data: { objects: [objectData] } };
}

function Pause_getObjectProperty({ object, name }) {
  const dbgObject = getObjectFromId(object);
  const completion = dbgObject.getProperty(name);
  return { result: completionToProtocolResult(completion) };
}

function Pause_getScope({ scope }) {
  const scopeData = createProtocolScope(scope);
  return { data: { scopes: [scopeData] } };
}

///////////////////////////////////////////////////////////////////////////////
// CSS Commands
///////////////////////////////////////////////////////////////////////////////

// This set is the intersection of the elements described at [1] and the
// elements which the firefox devtools server actually operates on [2].
//
// [1] https://developer.mozilla.org/en-US/docs/Web/CSS/Pseudo-elements
// [2] PSEUDO_ELEMENTS in devtools/shared/css/generated/properties-db.js
const PseudoElements = [
  ":after",
  ":backdrop",
  ":before",
  ":cue",
  ":first-letter",
  ":first-line",
  ":marker",
  ":placeholder",
  ":selection",
];

function addRules(rules, node, pseudoElement) {
  const baseRules = InspectorUtils.getCSSStyleRules(node, pseudoElement);

  // getCSSStyleRules returns rules in increasing order of specificity.
  // We need to return rules ordered in the opposite way.
  baseRules.reverse();

  for (const rule of baseRules) {
    rules.push({ rule: getObjectIdRaw(rule), pseudoElement });
  }
}

function CSS_getAppliedRules({ node }) {
  const nodeObj = getObjectFromId(node).unsafeDereference();
  if (nodeObj.nodeType != Node.ELEMENT_NODE) {
    return { rules: [], data: {} };
  }

  if (getPseudoType(node)) {
    // Don't return rules for the pseudo-element itself. These can be obtained
    // from the parent node's applied rules.
    return { rules: [], data: {} };
  }

  const rules = [];

  addRules(rules, nodeObj);
  for (const pseudoElement of PseudoElements) {
    addRules(rules, nodeObj, pseudoElement);
  }

  return { rules, data: {} };
}

function CSS_getComputedStyle({ node }) {
  const nodeObj = getObjectFromId(node).unsafeDereference();
  if (nodeObj.nodeType != Node.ELEMENT_NODE || !nodeObj.ownerGlobal) {
    return { computedStyle: [] };
  }

  const pseudoType = getPseudoType(node);

  let styleInfo;
  if (pseudoType) {
    styleInfo = nodeObj.ownerGlobal.getComputedStyle(
      nodeObj.parentNode,
      pseudoType
    );
  } else {
    styleInfo = nodeObj.ownerGlobal.getComputedStyle(nodeObj);
  }

  const computedStyle = [];
  for (let i = 0; i < styleInfo.length; i++) {
    computedStyle.push({
      name: styleInfo.item(i),
      value: styleInfo.getPropertyValue(styleInfo.item(i)),
    });
  }
  return { computedStyle };
}

function Target_getSheetSourceMapURL({ sheet }) {
  const sheetObj = getObjectFromId(sheet).unsafeDereference();
  return getSourceMapData(sheetObj) || {};
}

function Target_topFrameLocation() {
  let frame = gDebugger.getNewestFrame();
  while (frame && !considerScript(frame.script)) {
    frame = frame.older;
  }
  if (!frame) {
    return {};
  }
  return { location: getFrameLocation(frame) };
}

///////////////////////////////////////////////////////////////////////////////
// DOM Commands
///////////////////////////////////////////////////////////////////////////////

function shiftRect(rect, offset) {
  return {
    left: rect.left !== undefined ? offset.left + rect.left : undefined,
    top: rect.top !== undefined ? offset.top + rect.top : undefined,
    right: rect.right !== undefined ? offset.left + rect.right : undefined,
    bottom: rect.bottom !== undefined ? offset.top + rect.bottom : undefined,
  };
}

function DOM_getAllBoundingClientRects() {
  const window = getWindow();
  const cx = new StackingContext(window);

  cx.addChildren(window.document);

  const entries = cx.flatten();

  // Get elements in front-to-back order.
  entries.reverse();

  const elements = entries
    .map(elem => {
      const id = getObjectIdRaw(elem.raw);

      const { left, top, right, bottom } = shiftRect(elem.raw.getBoundingClientRect(), elem.offset);
      if (left >= right || top >= bottom) {
        return null;
      }

      const clipBounds = shiftRect(elem.clipBounds, elem.offset);
      // ignore elements that are completely outside their clipBounds
      if (
        clipBounds.left > right ||
        clipBounds.top > bottom ||
        clipBounds.right < left ||
        clipBounds.bottom < top
      ) {
        return null;
      }
      // only return the clipBounds that actually affect this element
      if (clipBounds.left === undefined || clipBounds.left <= left) {
        delete clipBounds.left;
      }
      if (clipBounds.top === undefined || clipBounds.top <= top) {
        delete clipBounds.top;
      }
      if (clipBounds.right === undefined || clipBounds.right >= right) {
        delete clipBounds.right;
      }
      if (clipBounds.bottom === undefined || clipBounds.bottom >= bottom) {
        delete clipBounds.bottom;
      }

      const rects = [...elem.raw.getClientRects()]
        .map(rect => shiftRect(rect, elem.offset))
        .map(({ left, top, right, bottom }) => {
          if (left >= right || top >= bottom) {
            return null;
          }
          return [left, top, right, bottom];
        })
        .filter((v) => !!v);

      const v = {
        node: id,
        rect: [left, top, right, bottom],
      };
      if (rects.length > 1) {
        v.rects = rects;
      }
      if (Object.keys(clipBounds).length > 0) {
        v.clipBounds = clipBounds;
      }
      if (elem.style?.getPropertyValue("visibility") === "hidden") {
        v.visibility = "hidden";
      }
      if (elem.style?.getPropertyValue("pointer-events") === "none") {
        v.pointerEvents = "none";
      }
      return v;
    })
    .filter((v) => !!v);

  return { elements };
}

function DOM_getBoundingClientRect({ node }) {
  const nodeObj = getObjectFromId(node).unsafeDereference();
  if (!nodeObj.getBoundingClientRect) {
    return { rect: [0, 0, 0, 0] };
  }

  const { left, top, right, bottom } = nodeObj.getBoundingClientRect();
  return { rect: [left, top, right, bottom] };
}

function DOM_getBoxModel({ node }) {
  const nodeObj = getObjectFromId(node).unsafeDereference();

  const model = { node };
  for (const box of ["content", "padding", "border", "margin"]) {
    const compactQuads = [];
    if (nodeObj.getBoxQuads) {
      const quads = nodeObj.getBoxQuads({
        box,
        relativeTo: getWindow().document,
      });
      for (const { p1, p2, p3, p4 } of quads) {
        compactQuads.push(p1.x, p1.y, p2.x, p2.y, p3.x, p3.y, p4.x, p4.y);
      }
    }
    model[box] = compactQuads;
  }
  return { model };
}

function DOM_getDocument() {
  const document = getObjectIdRaw(getWindow().document);
  return { document, data: {} };
}

function unwrapXray(obj) {
  if (Cu.isXrayWrapper(obj)) {
    return obj.wrappedJSObject;
  }
  return obj;
}

function DOM_getEventListeners({ node }) {
  const listeners = [];

  const nodeObj = getObjectFromId(node).unsafeDereference();
  const listenerInfo = Services.els.getListenerInfoFor(nodeObj) || [];
  if (nodeObj.nodeName && nodeObj.nodeName == "HTML") {
    // Add event listeners for the document and window as well.
    listenerInfo.push(
      ...Services.els.getListenerInfoFor(nodeObj.parentNode),
      ...Services.els.getListenerInfoFor(nodeObj.ownerGlobal)
    );
  }

  for (const { type, listenerObject, capturing } of listenerInfo) {
    const handler = unwrapXray(listenerObject);
    if (!handler) {
      continue;
    }
    const dbgHandler = makeDebuggeeValue(handler);
    if (dbgHandler.class != "Function") {
      continue;
    }
    const id = getObjectId(dbgHandler);
    listeners.push({
      node,
      handler: id,
      type,
      capture: capturing,
    });
  }

  return { listeners, data: {} };
}

function newTreeWalker() {
  const walker = Cc["@mozilla.org/inspector/deep-tree-walker;1"].createInstance(
    Ci.inIDeepTreeWalker
  );
  walker.showAnonymousContent = true;
  walker.showSubDocuments = true;
  walker.showDocumentsAsNodes = true;
  walker.init(getWindow().document, 0xffffffff);
  return walker;
}

function forAllNodes(callback) {
  const walker = newTreeWalker();
  for (let node = walker.currentNode; node; node = walker.nextNode()) {
    callback(node);
  }
}

// Get the raw DOM nodes containing a query string.
function searchDOM(query) {
  const rv = [];
  forAllNodes(addEntries);
  return rv;

  function checkText(node, text) {
    if (text.includes(query)) {
      if (!rv.length || rv[rv.length - 1] != node) {
        rv.push(node);
      }
    }
  }

  function addEntries(node) {
    if (node.nodeType == Node.ELEMENT_NODE) {
      checkText(node, convertNodeName(node.localName));
      for (const { name, value } of node.attributes) {
        checkText(node, name);
        checkText(node, value);
      }
    } else {
      checkText(node, node.textContent || "");
    }
  }

  function convertNodeName(name) {
    switch (name) {
      case "_moz_generated_content_marker":
        return "::marker";
      case "_moz_generated_content_before":
        return "::before";
      case "_moz_generated_content_after":
        return "::after";
    }
    return name;
  }
}

function DOM_performSearch({ query }) {
  const rawNodes = searchDOM(query);
  const nodes = rawNodes.map(getObjectIdRaw);
  return { nodes, data: {} };
}

function DOM_querySelector({ node, selector }) {
  const nodeObj = getObjectFromId(node).unsafeDereference();

  const resultObj = nodeObj.querySelector(selector);
  if (!resultObj) {
    return { data: {} };
  }
  const result = getObjectIdRaw(resultObj);
  return { result, data: {} };
}
