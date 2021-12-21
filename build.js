const fs = require("fs");
const path = require("path");
const { spawnSync } = require("child_process");
const gecko = __dirname;

// Download the record/replay driver archive, using the latest version unless
//it was overridden via the environment.
const driverArchive = `${currentPlatform()}-recordreplay.tgz`;
let downloadArchive = driverArchive;
if (process.env.DRIVER_REVISION) {
  downloadArchive = `${currentPlatform()}-recordreplay-${process.env.DRIVER_REVISION}.tgz`;
}
spawnChecked("curl", [`https://static.replay.io/downloads/${downloadArchive}`, "-o", driverArchive], { stdio: "inherit" });

// Files which should be in the archive.
const driverFile = `${currentPlatform()}-recordreplay.${driverExtension()}`;
const driverJSON = `${currentPlatform()}-recordreplay.json`;
spawnChecked("tar", ["xf", driverArchive]);
fs.unlinkSync(driverArchive);

// Embed the driver in the source.
const driverContents = fs.readFileSync(driverFile);
const { revision: driverRevision, date: driverDate } = JSON.parse(fs.readFileSync(driverJSON, "utf8"));
fs.unlinkSync(driverFile);
fs.unlinkSync(driverJSON);
let driverString = "";
for (let i = 0; i < driverContents.length; i++) {
  driverString += `\\${driverContents[i].toString(8)}`;
}
fs.writeFileSync(
  path.join(gecko, "toolkit", "recordreplay", "RecordReplayDriver.cpp"),
  `
namespace mozilla::recordreplay {
  char gRecordReplayDriver[] = "${driverString}";
  int gRecordReplayDriverSize = ${driverContents.length};
  char gBuildId[] = "${computeBuildId()}";
}
  `
);

const buildOptions = {
  stdio: "inherit",
  env: {
    ...process.env,
    RUSTC_BOOTSTRAP: "qcms",
    // terminal-notifier can hang, so prevent it from running.
    MOZ_NOSPAM: "1",
  },
};

if (currentPlatform() == "windows") {
  // Windows builds need to enter the mozilla-build shell, and uses separate
  // scripts for this.
  spawnChecked(".\\windows-build.bat", [], buildOptions);
} else {
  spawnChecked("./mach", ["build"], buildOptions);
  spawnChecked("./mach", ["package"], buildOptions);
}

function spawnChecked(cmd, args, options) {
  const prettyCmd = [cmd].concat(args).join(" ");
  console.error(prettyCmd);

  const rv = spawnSync(cmd, args, options);

  if (rv.status != 0 || rv.error) {
    console.error(rv.error);
    throw new Error(`Spawned process failed with exit code ${rv.status}`);
  }

  return rv;
}

function currentPlatform() {
  switch (process.platform) {
    case "darwin":
      return "macOS";
    case "linux":
      return "linux";
    case "win32":
      return "windows";
    default:
      throw new Error(`Platform ${process.platform} not supported`);
  }
}

function driverExtension() {
  return currentPlatform() == "windows" ? "dll" : "so";
}

function computeBuildId() {
  const geckoRevision = spawnChecked("git", ["rev-parse", "--short", "HEAD"]).stdout.toString().trim();
  const geckoDate = spawnChecked("git", [
    "show",
    "HEAD",
    "--pretty=%cd",
    "--date=short",
    "--no-patch",
  ])
    .stdout.toString()
    .trim()
    .replace(/-/g, "");

  // Use the later of the two dates in the build ID.
  const date = +geckoDate >= +driverDate ? geckoDate : driverDate;

  return `${currentPlatform()}-gecko-${date}-${geckoRevision}-${driverRevision}`;
}
