var EXPORTED_SYMBOLS = ["pingTelemetry"];

const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");

function pingTelemetry(source, name, data) {
  const url = Services.prefs.getStringPref("replay.telemetry.url");
  const enabled = Services.prefs.getBoolPref("replay.telemetry.enabled");

  if (!enabled || !url) return;

  fetch(url, {
    method: 'POST',
    body: JSON.stringify({
      ...data,
      event: 'Gecko',
      build: Services.appinfo.appBuildID,
      ts: Date.now(),
      source,
      name
    })
  }).catch(console.error);
}