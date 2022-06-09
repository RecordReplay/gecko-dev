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

function getChannelResponseData(channel, fromCache, fromServiceWorker) {
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
    responseFromServiceWorker: !!fromServiceWorker,
    remoteDestination: (fromServiceWorker || fromCache) ? null : {
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

const FAILED_BLOCKED_REASONS = new Map([
  // See nsILoadInfo.idl for this list.
  [Ci.nsILoadInfo.BLOCKING_REASON_CORSDISABLED, "CORS_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CORSDIDNOTSUCCEED, "CORS_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CORSREQUESTNOTHTTP, "CORS_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CORSMULTIPLEALLOWORIGINNOTALLOWED, "CORS_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CORSMISSINGALLOWORIGIN, "CORS_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CORSNOTSUPPORTINGCREDENTIALS, "CORS_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CORSALLOWORIGINNOTMATCHINGORIGIN, "CORS_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CORSMISSINGALLOWCREDENTIALS, "CORS_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CORSORIGINHEADERNOTADDED, "CORS_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CORSEXTERNALREDIRECTNOTALLOWED, "CORS_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CORSPREFLIGHTDIDNOTSUCCEED, "CORS_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CORSINVALIDALLOWMETHOD, "CORS_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CORSMETHODNOTFOUND, "CORS_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CORSINVALIDALLOWHEADER, "CORS_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CORSMISSINGALLOWHEADERFROMPREFLIGHT, "CORS_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CLASSIFY_MALWARE_URI, "CLASSIFIER_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CLASSIFY_PHISHING_URI, "CLASSIFIER_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CLASSIFY_UNWANTED_URI, "CLASSIFIER_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CLASSIFY_TRACKING_URI, "CLASSIFIER_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CLASSIFY_BLOCKED_URI, "CLASSIFIER_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CLASSIFY_HARMFUL_URI, "CLASSIFIER_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CLASSIFY_CRYPTOMINING_URI, "CLASSIFIER_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CLASSIFY_FINGERPRINTING_URI, "CLASSIFIER_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CLASSIFY_SOCIALTRACKING_URI, "CLASSIFIER_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_MIXED_BLOCKED, "MIXED_CONTENT_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CONTENT_POLICY_GENERAL, "CONTENT_POLICY_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CONTENT_POLICY_NO_DATA_PROTOCOL, "CONTENT_POLICY_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CONTENT_POLICY_WEBEXT, "CONTENT_POLICY_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CONTENT_POLICY_CONTENT_BLOCKED, "CONTENT_POLICY_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CONTENT_POLICY_DATA_DOCUMENT, "CONTENT_POLICY_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CONTENT_POLICY_WEB_BROWSER, "CONTENT_POLICY_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_CONTENT_POLICY_PRELOAD, "CONTENT_POLICY_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_NOT_SAME_ORIGIN, "ORIGIN_MISMATCH_BLOCKED"],
  [Ci.nsILoadInfo.BLOCKING_REASON_EXTENSION_WEBREQUEST, "EXTENSION_BLOCKED"],
]);

function getChannelRequestFailedData(channel) {
  let requestFailedReason;
  if (channel.loadInfo.requestBlockingReason !== Ci.nsILoadInfo.BLOCKING_REASON_NONE) {
    requestFailedReason = FAILED_BLOCKED_REASONS.get(channel.loadInfo.requestBlockingReason) ?? "BLOCKED";
  } else if (channel.status !== Cr.NS_OK) {
    // Return one of Firefox's internal NS_FOO error strings.
    requestFailedReason = `0x${channel.status.toString(16)}`;
  }

  return {
    requestFailedReason,
  };
}

// eslint-disable-next-line no-unused-vars
var EXPORTED_SYMBOLS = [
  "getChannelRequestData",
  "getChannelResponseData",
  "getChannelRequestDoneData",
  "getChannelRequestFailedData",
];
