/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The functions in this file have primarily been copied from
// 'devtools/server/actors/webconsole/eval-with-debugger.js'. We had to
// duplicate them in order to ensure that they run in the correct compartment.

const { require } = ChromeUtils.import("resource://devtools/shared/Loader.jsm");

const eagerEcmaAllowlist = require("devtools/server/actors/webconsole/eager-ecma-allowlist");
const eagerFunctionAllowlist = require("devtools/server/actors/webconsole/eager-function-allowlist");

const EXPORTED_SYMBOLS = ["evalExpression"];

function evalExpression(dbg, dbgGlobal, frame, expression, pure) {
  let noSideEffectDebugger = null;
  if (pure) {
    noSideEffectDebugger = makeSideeffectFreeDebugger(dbg);

    // Bug 1637883 demonstrated an issue where dbgGlobal was somehow in the
    // same compartment as the Debugger, meaning it could not be debugged
    // and thus cannot handle eager evaluation. In that case we skip execution.
    if (!noSideEffectDebugger.hasDebuggee(dbgGlobal.unsafeDereference())) {
      return null;
    }

    // When a sideeffect-free debugger has been created, we need to eval
    // in the context of that debugger in order for the side-effect tracking
    // to apply.
    frame = frame ? noSideEffectDebugger.adoptFrame(frame) : null;
    dbgGlobal = noSideEffectDebugger.adoptDebuggeeValue(dbgGlobal);
  }

  let completion;
  try {
    if (frame) {
      completion = frame.eval(expression);
    } else {
      completion = dbgGlobal.executeInGlobal(expression);
    }

    if (noSideEffectDebugger && completion) {
      if ("return" in completion) {
        completion.return = dbg.adoptDebuggeeValue(completion.return);
      }
      if ("throw" in completion) {
        completion.throw = dbg.adoptDebuggeeValue(completion.throw);
      }
    }
  } finally {
    // We need to be absolutely sure that the sideeffect-free debugger's
    // debuggees are removed because otherwise we risk them terminating
    // execution of later code in the case of unexpected exceptions.
    if (noSideEffectDebugger) {
      noSideEffectDebugger.removeAllDebuggees();
    }
  }

  return completion;
}

function makeSideeffectFreeDebugger(originalDbg) {
  // We ensure that the metadata for native functions is loaded before we
  // initialize sideeffect-prevention because the data is lazy-loaded, and this
  // logic can run inside of debuggee compartments because the
  // "addAllGlobalsAsDebuggees" considers the vast majority of realms
  // valid debuggees. Without this, eager-eval runs the risk of failing
  // because building the list of valid native functions is itself a
  // side-effectful operation because it needs to populate a
  // module cache, among any number of other things.
  ensureSideEffectFreeNatives();

  // Note: It is critical for debuggee performance that we implement all of
  // this debuggee tracking logic with a separate Debugger instance.
  // Bug 1617666 arises otherwise if we set an onEnterFrame hook on the
  // existing debugger object and then later clear it.
  const dbg = new originalDbg.constructor();
  dbg.addAllGlobalsAsDebuggees();

  const timeoutDuration = 100;
  const endTime = Date.now() + timeoutDuration;
  let count = 0;
  function shouldCancel() {
    // To keep the evaled code as quick as possible, we avoid querying the
    // current time on ever single step and instead check every 100 steps
    // as an arbitrary count that seemed to be "often enough".
    return ++count % 100 === 0 && Date.now() > endTime;
  }

  const executedScripts = new Set();
  const handler = {
    hit: () => null,
  };
  dbg.onEnterFrame = frame => {
    if (shouldCancel()) {
      return null;
    }
    frame.onStep = () => {
      if (shouldCancel()) {
        return null;
      }
      return undefined;
    };

    const script = frame.script;

    if (executedScripts.has(script)) {
      return undefined;
    }
    executedScripts.add(script);

    const offsets = script.getEffectfulOffsets();
    for (const offset of offsets) {
      script.setBreakpoint(offset, handler);
    }

    return undefined;
  };

  // The debugger only calls onNativeCall handlers on the debugger that is
  // explicitly calling eval, so we need to add this hook on "dbg" even though
  // the rest of our hooks work via "newDbg".
  dbg.onNativeCall = (callee, reason) => {
    try {
      // Getters are never considered effectful, and setters are always effectful.
      // Natives called normally are handled with an allowlist.
      if (
        reason == "get" ||
        (reason == "call" && nativeHasNoSideEffects(callee))
      ) {
        // Returning undefined causes execution to continue normally.
        return undefined;
      }
    } catch (err) {
      log(
        "evalWithDebugger onNativeCall: Unable to validate native function against allowlist"
      );
    }
    // Returning null terminates the current evaluation.
    return null;
  };

  return dbg;
}

// Native functions which are considered to be side effect free.
let gSideEffectFreeNatives; // string => Array(Function)

function ensureSideEffectFreeNatives() {
  if (gSideEffectFreeNatives) {
    return;
  }

  const natives = [
    ...eagerEcmaAllowlist,

    // Pull in all of the non-ECMAScript native functions that we want to
    // allow as well.
    ...eagerFunctionAllowlist,
  ];

  const map = new Map();
  for (const n of natives) {
    if (!map.has(n.name)) {
      map.set(n.name, []);
    }
    map.get(n.name).push(n);
  }

  gSideEffectFreeNatives = map;
}

function nativeHasNoSideEffects(fn) {
  if (fn.isBoundFunction) {
    fn = fn.boundTargetFunction;
  }

  // Natives with certain names are always considered side effect free.
  switch (fn.name) {
    case "toString":
    case "toLocaleString":
    case "valueOf":
      return true;
  }

  const natives = gSideEffectFreeNatives.get(fn.name);
  return natives && natives.some(n => fn.isSameNative(n));
}
