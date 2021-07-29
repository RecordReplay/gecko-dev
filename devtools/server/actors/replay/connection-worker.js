/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/* eslint-disable spaced-comment, brace-style, indent-legacy, no-shadow */

"use strict";

// Sockets for communicating with the Record Replay cloud service.
const gSockets = new Map();

self.addEventListener("message", makeInfallible(onMainThreadMessage));

function onMainThreadMessage({ data }) {
  switch (data.kind) {
    case "openChannel": {
      const socket = new ProtocolSocket(data.address);
      gSockets.set(data.channelId, socket);
      socket.onStateChange = state => {
        postMessage({
          kind: "stateChange",
          channelId: data.channelId,
          state,
        });
      };
      break;
    }
    case "sendCommand": {
      gSockets.get(data.channelId).sendCommand(data.method, data.params)
        .catch(err => {
          postError(`UnexpectedCommandException: ${err}`);
          return {
            result: null,
            error: {
              code: -2,
              message: "Unexpected internal error"
            }
          };
        })
        .then(({ result, error }) => {
          postMessage({
            kind: "commandResponse",
            channelId: data.channelId,
            commandId: data.commandId,
            result,
            error
          });
        });
      break;
    }
    case "closeChannel": {
      const socket = gSockets.get(data.channelId);
      if (socket) {
        gSockets.delete(data.channelId);
        socket.close();
      }
      break;
    }
    default:
      postError(`Unknown event kind ${data.kind}`);
  }
}

// Every upload uses its own socket. This allows other communication with the
// cloud service even if the upload socket has a lot of pending data to send.
class ProtocolSocket {
  constructor(address) {
    this._address = address;
    this._state = "connecting";
    this._msgId = 1;
    this._pendingMessages = new Map();
    this._socket = null;
    this._onStateChangeCallback = null;

    this._initialize();
  }

  get onStateChange() {
    return this._onStateChangeCallback;
  }

  set onStateChange(callback) {
    this._onStateChangeCallback = callback;
    this._notifyStateChange();
  }

  _initialize() {
    this._socket = new WebSocket(this._address);
    this._socket.onopen = makeInfallible(() => this._onOpen());
    this._socket.onclose = makeInfallible(() => this._onClose());
    this._socket.onmessage = makeInfallible(evt => this._onServerMessage(evt));
    this._socket.onerror = makeInfallible(() => this._onError());
  }

  _onOpen() {
    this._msgId = 1;
    this._state = "open";

    for (const entry of this._pendingMessages.values()) {
      if (typeof entry.msg !== "string") {
        postError("Unexpected non-pending message");
        return;
      }
      doSend(this._socket, entry.msg);
      entry.msg = null;
    }

    this._notifyStateChange();
  }

  _onClose() {
    if (this._state === "closed") {
      return;
    }

    for (const entry of this._pendingMessages.values()) {
      entry.resolve(makeUnavailableResponse());
    }
    this._pendingMessages.clear();

    this._state = "connecting";
    this._notifyStateChange();
    setTimeout(() => this._initialize(), 3000);
  }

  _onError() {
    this._state = "error";
    this._notifyStateChange();
  }

  _notifyStateChange() {
    this._onStateChangeCallback?.(this._state);
  }

  _onServerMessage(evt) {
    const data = JSON.parse(evt.data);
    if (data.id == null) {
      // Ignore events.
      return;
    }

    const entry = this._pendingMessages.get(data.id);
    this._pendingMessages.delete(data.id);
    if (!entry) {
      postError(`Unexpected response id ${data.id}`);
      return;
    }
    entry.resolve(data);
  }

  async sendCommand(method, params = {}) {
    if (this._state !== "open" && this._state !== "connecting") {
      return makeUnavailableResponse();
    }

    const result = await new Promise(resolve => {
      const msgId = this._msgId++;
      const entry = {
        msgId,
        resolve,
        msg: JSON.stringify({
          id: msgId,
          method,
          params,
        }),
      };
      this._pendingMessages.set(msgId, entry);

      if (this._state === "open") {
        doSend(this._socket, entry.msg);
        entry.msg = null;
      }
    });

    return {
      error: result.error,
      result: result.result,
    };
  }

  close() {
    for (const entry of this._pendingMessages.values()) {
      entry.resolve(makeUnavailableResponse());
    }
    this._pendingMessages.clear();

    this._state = "closed";
    this._socket.close();
    this._notifyStateChange();
  }
}

function makeUnavailableResponse() {
  return {
    result: null,
    error: {
      code: -1,
      message: "Connection Unavailable",
    },
  };
}

const doSend = makeInfallible((socket, msg) => socket.send(msg));

function makeInfallible(fn) {
  return (...args) => {
    try {
      fn(...args);
    } catch (e) {
      postError(e);
    }
  };
}

function postError(msg) {
  dump(`Error: ${msg}\n`);
}
