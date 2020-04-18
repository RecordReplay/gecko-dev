/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */
/* eslint-disable no-undef */

"use strict";

Services.scriptloader.loadSubScript(
  "chrome://mochitests/content/browser/devtools/client/shared/test/shared-head.js",
  this
);

Services.scriptloader.loadSubScript(
  "chrome://mochitests/content/browser/devtools/client/debugger/test/mochitest/helpers.js",
  this
);

const EXAMPLE_URL =
  "http://example.com/browser/devtools/client/webreplay/mochitest/examples/";

// Attach a debugger to a tab, returning a promise that resolves with the
// debugger's toolbox.
async function attachDebugger(tab) {
  const target = await TargetFactory.forTab(tab);
  const toolbox = await gDevTools.showToolbox(target, "jsdebugger");
  const dbg = createDebuggerContext(toolbox);
  const threadFront = dbg.toolbox.threadFront;
  return { ...dbg, tab, threadFront };
}

async function openRecordingTab(url) {
  const tab = BrowserTestUtils.addTab(gBrowser, null, { recordExecution: "*" });
  gBrowser.selectedTab = tab;
  await once(Services.ppmm, "RecordingInitialized");
  openTrustedLinkIn(EXAMPLE_URL + url, "current");
  return tab;
}

async function attachRecordingDebugger(
  url,
  { waitForRecording, skipInterrupt } = {}
) {
  await pushPref("devtools.recordreplay.logging", true);

  const tab = await openRecordingTab(url);

  if (waitForRecording) {
    await once(Services.ppmm, "RecordingFinished");
  }
  const dbg = await attachDebugger(tab);

  if (!skipInterrupt) {
    await interrupt(dbg);
  }

  return dbg;
}

async function waitForPausedNoSource(dbg) {
  await waitForState(dbg, state => isPaused(dbg), "paused");
}

async function shutdownDebugger(dbg, removeTab = true) {
  await dbg.actions.removeAllBreakpoints(getContext(dbg));
  await waitForRequestsToSettle(dbg);
  await dbg.toolbox.destroy();
  if (removeTab) {
    await gBrowser.removeTab(dbg.tab);
  }
}

async function interrupt(dbg) {
  await dbg.actions.breakOnNext(getThreadContext(dbg));
  await waitForPausedNoSource(dbg);
}

function resumeThenPauseAtLineFunctionFactory(method) {
  return async function(dbg, lineno) {
    await dbg.actions[method](getThreadContext(dbg));
    if (lineno !== undefined) {
      await waitForPaused(dbg);
    } else {
      await waitForPausedNoSource(dbg);
    }
    const pauseLine = getVisibleSelectedFrameLine(dbg);
    ok(pauseLine == lineno, `Paused at line ${pauseLine} expected ${lineno}`);
  };
}

// Define various methods that resume a thread in a specific way and ensure it
// pauses at a specified line.
var rewindToLine = resumeThenPauseAtLineFunctionFactory("rewind");
var resumeToLine = resumeThenPauseAtLineFunctionFactory("resume");
var reverseStepOverToLine = resumeThenPauseAtLineFunctionFactory(
  "reverseStepOver"
);
var stepOverToLine = resumeThenPauseAtLineFunctionFactory("stepOver");
var stepInToLine = resumeThenPauseAtLineFunctionFactory("stepIn");
var stepOutToLine = resumeThenPauseAtLineFunctionFactory("stepOut");

// Return a promise that resolves when a thread evaluates a string in the
// topmost frame, with the result throwing an exception.
async function checkEvaluateInTopFrameThrows(dbg, text) {
  const threadFront = dbg.toolbox.target.threadFront;
  const consoleFront = await dbg.toolbox.target.getFront("console");
  const { frames } = await threadFront.getFrames(0, 1);
  ok(frames.length == 1, "Got one frame");
  const options = { thread: threadFront.actor, frameActor: frames[0].actorID };
  const response = await consoleFront.evaluateJSAsync(text, options);
  ok(response.exception, "Eval threw an exception");
}

// Return a pathname that can be used for a new recording file.
function newRecordingFile() {
  const { OS } = ChromeUtils.import("resource://gre/modules/osfile.jsm");
  return OS.Path.join(
    OS.Constants.Path.tmpDir,
    "MochitestRecording" + Math.round(Math.random() * 1000000000)
  );
}

function findMessage(hud, text, selector = ".message") {
  const messages = findMessages(hud, text, selector);
  return messages ? messages[0] : null;
}

function findMessages(hud, text, selector = ".message") {
  const messages = hud.ui.outputNode.querySelectorAll(selector);
  const elements = Array.prototype.filter.call(messages, el =>
    el.textContent.includes(text)
  );

  if (elements.length == 0) {
    return null;
  }

  return elements;
}

function waitForMessage(hud, text, selector = ".message") {
  return waitUntilPredicate(() => findMessage(hud, text, selector));
}

function waitForMessages(hud, text, selector = ".message") {
  return waitUntilPredicate(() => findMessages(hud, text, selector));
}

async function waitForMessageCount(hud, text, length, selector = ".message") {
  let messages;
  await waitUntil(() => {
    messages = findMessages(hud, text, selector);
    return messages && messages.length == length;
  });
  ok(messages.length == length, "Found expected message count");
  return messages;
}

async function warpToMessage(hud, dbg, text, maybeLine) {
  let messages = await waitForMessages(hud, text);
  ok(messages.length == 1, "Found one message");
  const message = messages.pop();

  const menuPopup = await openConsoleContextMenu(message);
  console.log(`.>> menu`, menuPopup);

  const timeWarpItem = menuPopup.querySelector("#console-menu-time-warp");
  ok(timeWarpItem, "Time warp menu item is available");

  timeWarpItem.click();

  await hideConsoleContextMenu();
  await waitForPaused(dbg);

  messages = findMessages(hud, "", ".paused");
  ok(messages.length == 1, "Found one paused message");

  if (maybeLine) {
    await waitUntil(() => maybeLine == getVisibleSelectedFrameLine(dbg));
  }

  return message;

  async function openConsoleContextMenu(element) {
    const onConsoleMenuOpened = hud.ui.wrapper.once("menu-open");
    synthesizeContextMenuEvent(element);
    await onConsoleMenuOpened;
    return dbg.toolbox.topDoc.getElementById("webconsole-menu");
  }

  function hideConsoleContextMenu() {
    const popup = dbg.toolbox.topDoc.getElementById("webconsole-menu");
    if (!popup) {
      return Promise.resolve();
    }

    const onPopupHidden = once(popup, "popuphidden");
    popup.hidePopup();
    return onPopupHidden;
  }
}

// For tests that need webconsole test features.
const BrowserTest = {
  gTestPath,
  ok,
  is,
  registerCleanupFunction,
  waitForExplicitFinish,
  BrowserTestUtils,
};

Services.scriptloader.loadSubScript(
  "chrome://mochitests/content/browser/devtools/client/webconsole/test/browser/head.js",
  BrowserTest
);

async function checkMessageObjectContents(msg, expected, expandList = []) {
  const oi = msg.querySelector(".tree");
  const node = oi.querySelector(".tree-node");
  BrowserTest.expandObjectInspectorNode(node);

  for (const label of expandList) {
    const labelNode = await waitFor(() =>
      BrowserTest.findObjectInspectorNode(oi, label)
    );
    BrowserTest.expandObjectInspectorNode(labelNode);
  }

  await waitFor(() => {
    const nodes = BrowserTest.getObjectInspectorNodes(oi);
    if (nodes && nodes.length > 1) {
      const properties = [...nodes].map(n => n.textContent);
      return expected.every(s => properties.find(v => v.includes(s)));
    }
    return null;
  });

  ok("Got expected object contents");
}

async function findNode(dbg, text) {
  let node;
  await waitUntil(() => {
    for (let index = 0; index < 100; index++) {
      const elem = findElement(dbg, "scopeNode", index);
      if (elem && elem.innerText == text) {
        node = elem;
        return true;
      }
    }
  });
  return node;
}

async function toggleNode(dbg, text) {
  const node = await findNode(dbg, text);
  return toggleObjectInspectorNode(node);
}

function findNodeValue(dbg, text) {
  for (let index = 0; index < 100; index++) {
    var elem = findElement(dbg, "scopeNode", index);
    if (elem && elem.innerText == text) {
      return findElement(dbg, "scopeValue", index).innerText;
    }
  }
}

async function waitForNodeValue(dbg, name, value) {
  await findNode(dbg, name);
  await waitUntil(() => findNodeValue(dbg, name) == value);
}

function waitForInstantStep(dbg, type) {
  const point = dbg.selectors.getThreadExecutionPoint(dbg.selectors.getCurrentThread());
  return waitUntil(() => dbg.client.eventMethods.canInstantStep(point, type));
}

async function checkInlinePreview(dbg, obj) {
  await waitUntil(() => dbg.selectors.getSelectedInlinePreviews());
  const previews = dbg.selectors.getSelectedInlinePreviews();
  ok(JSON.stringify(previews).includes(JSON.stringify(obj)), "correct inline preview contents");
}

PromiseTestUtils.whitelistRejectionsGlobally(/NS_ERROR_NOT_INITIALIZED/);
PromiseTestUtils.whitelistRejectionsGlobally(/Error in asyncStorage/);
