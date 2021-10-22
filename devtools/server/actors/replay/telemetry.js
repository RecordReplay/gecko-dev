var EXPORTED_SYMBOLS = ["pingTelemetry"];

const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");
const ReplayAuth = ChromeUtils.import(
  "resource://devtools/server/actors/replay/auth.js"
);

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

  const xhr = new XMLHttpRequest();
  xhr.open("POST", url);
  if (auth) {
    xhr.setRequestHeader("Authorization", `Bearer ${auth}`);
  }
  xhr.send(JSON.stringify({
    ...data,
    event: 'Gecko',
    build: Services.appinfo.appBuildID,
    ts: Date.now(),
    source,
    name,
    browserSettings,
  }));

  xhr.onerror = console.error;
}
