/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/* eslint-disable spaced-comment, brace-style, indent-legacy, no-shadow */

// This file defines the helper functions to reach data from an http channel.

"use strict";

/**
 * Convert a nsIContentPolicy constant to a display string.
 * This list was copied from "devtools/server/actors/network-monitor/utils/network-utils.js".
 */
 const LOAD_CAUSE_STRINGS = {
  [Ci.nsIContentPolicy.TYPE_INVALID]: "invalid",
  [Ci.nsIContentPolicy.TYPE_OTHER]: "other",
  [Ci.nsIContentPolicy.TYPE_SCRIPT]: "script",
  [Ci.nsIContentPolicy.TYPE_IMAGE]: "img",
  [Ci.nsIContentPolicy.TYPE_STYLESHEET]: "stylesheet",
  [Ci.nsIContentPolicy.TYPE_OBJECT]: "object",
  [Ci.nsIContentPolicy.TYPE_DOCUMENT]: "document",
  [Ci.nsIContentPolicy.TYPE_SUBDOCUMENT]: "subdocument",
  [Ci.nsIContentPolicy.TYPE_PING]: "ping",
  [Ci.nsIContentPolicy.TYPE_XMLHTTPREQUEST]: "xhr",
  [Ci.nsIContentPolicy.TYPE_OBJECT_SUBREQUEST]: "objectSubdoc",
  [Ci.nsIContentPolicy.TYPE_DTD]: "dtd",
  [Ci.nsIContentPolicy.TYPE_FONT]: "font",
  [Ci.nsIContentPolicy.TYPE_MEDIA]: "media",
  [Ci.nsIContentPolicy.TYPE_WEBSOCKET]: "websocket",
  [Ci.nsIContentPolicy.TYPE_CSP_REPORT]: "csp",
  [Ci.nsIContentPolicy.TYPE_XSLT]: "xslt",
  [Ci.nsIContentPolicy.TYPE_BEACON]: "beacon",
  [Ci.nsIContentPolicy.TYPE_FETCH]: "fetch",
  [Ci.nsIContentPolicy.TYPE_IMAGESET]: "imageset",
  [Ci.nsIContentPolicy.TYPE_WEB_MANIFEST]: "webManifest",
};

function getChannelRequestData(channel) {
  const requestHeaders = [];
  channel.visitRequestHeaders({
    visitHeader: (name, value) => requestHeaders.push({ name, value }),
  });

  return {
    requestUrl: channel.URI?.spec,
    requestMethod: channel.requestMethod,
    requestHeaders,
    requestCause: LOAD_CAUSE_STRINGS[channel.loadInfo?.externalContentPolicyType] || undefined,
  };
}

function getChannelResponseData(channel, fromCache) {
  const responseHeaders = [];
  channel.visitOriginalResponseHeaders({
    visitHeader: (name, value) => responseHeaders.push({ name, value }),
  });

  return {
    responseHeaders,
    responseProtocolVersion: channel.protocolVersion,
    responseStatus: channel.responseStatus,
    responseStatusText: channel.responseStatusText,
    responseFromCache: !!fromCache,
    remoteDestination: fromCache ? null : {
      address: channel.remoteAddress,
      port: channel.remotePort,
    },
  };
}

function getChannelRequestDoneData(channel) {
  let hasContentEncodings = false;
  try {
    hasContentEncodings = !!channel.getResponseHeader("Content-Encoding");
  } catch (err) {
    if (err?.result !== Cr.NS_ERROR_NOT_AVAILABLE) {
      throw err;
    }
  }

  return {
    // If there are not content encodings, the decodedBodySize is usually just 0.
    decodedBodySize: hasContentEncodings ? channel.decodedBodySize : undefined,
    encodedBodySize: channel.encodedBodySize,
  };
}

function getChannelRequestFailedData(channel) {
  return {
    requestFailedReason: channel.loadInfo.requestBlockingReason || undefined,
  };
}

// eslint-disable-next-line no-unused-vars
var EXPORTED_SYMBOLS = [
  "getChannelRequestData",
  "getChannelResponseData",
  "getChannelRequestDoneData",
  "getChannelRequestFailedData",
];
