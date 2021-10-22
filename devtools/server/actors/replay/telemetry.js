var EXPORTED_SYMBOLS = ["pingTelemetry"];

const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");
const ReplayAuth = ChromeUtils.import(
  "resource://devtools/server/actors/replay/auth.js"
);
const { XPCOMUtils } = ChromeUtils.import(
  "resource://gre/modules/XPCOMUtils.jsm"
);
XPCOMUtils.defineLazyGlobalGetters(this, ["fetch"]);

function pingTelemetry(source, name, data) {
  const url = Services.prefs.getStringPref("replay.telemetry.url");
  const enabled = Services.prefs.getBoolPref("replay.telemetry.enabled");

  if (!enabled || !url) return;

  // fetch the user info to send in `Authorization` header.
  const auth = ReplayAuth.getOriginalApiKey() || ReplayAuth.getReplayUserToken();

  // Collect info to send for `browserSettings` field.
  const disablePreallocated = Services.prefs.getBoolPref(
    "devtools.recordreplay.disablePreallocated"
  );
  const browserSettings = { usePreallocated: !disablePreallocated };

  fetch(url, {
    method: 'POST',
    headers: auth ? { Authorization: `Bearer ${auth}` } : undefined,
    body: JSON.stringify({
      ...data,
      event: 'Gecko',
      build: Services.appinfo.appBuildID,
      ts: Date.now(),
      source,
      name,
      browserSettings,
    })
  }).catch(console.error);
}