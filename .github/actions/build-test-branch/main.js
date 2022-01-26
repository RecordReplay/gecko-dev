
const {
  getLatestReplayRevision,
  sendBuildTestRequest,
  newTask,
} = require("../utils");

const branchName = getBranchName(process.env.GITHUB_REF);
console.log("BranchName", branchName);

const replayRevision = getLatestReplayRevision();

const driverRevision = process.env.INPUT_DRIVER_REVISION;
console.log("DriverRevision", driverRevision);

if (branchName.includes("webreplay-release") && !driverRevision) {
  console.error("Use build/test action for release branch");
  process.exit(1);
}

const clobberInput = process.env.INPUT_CLOBBER;
console.log("Clobber", clobberInput);
const clobber = clobberInput == "true";

const slotInput = process.env.INPUT_SLOT;
console.log("Slot", slotInput);
const slot = slotInput ? +slotInput : undefined;

const buildOnly = !!process.env.BUILD_ONLY;
console.log("BuildOnly", buildOnly);

let requestName = `Gecko Build/Test Branch ${branchName} ${replayRevision}`;
if (driverRevision) {
  requestName += ` driver ${driverRevision}`;
}
if (slot) {
  requestName += ` slot ${slot}`;
}

sendBuildTestRequest({
  name: requestName,
  tasks: [
    ...platformTasks("macOS"),
    ...platformTasks("linux"),
    ...platformTasks("windows"),
  ],
});

function platformTasks(platform) {
  const buildReplayTask = newTask(
    `Build Gecko ${platform}`,
    {
      kind: "BuildRuntime",
      runtime: "gecko",
      revision: replayRevision,
      branch: branchName,
      branchSlot: slot,
      driverRevision,
      clobber,
    },
    platform
  );

  const tasks = [buildReplayTask];

  if (!buildOnly) {
    const testReplayTask = newTask(
      `Run Tests ${platform}`,
      {
        kind: "StaticLiveTests",
        runtime: "gecko",
        revision: replayRevision,
        driverRevision,
      },
      platform,
      [buildReplayTask]
    );
    tasks.push(testReplayTask);
  }

  return tasks;
}

function getBranchName(refName) {
  // Strip everything after the last "/" from the ref to get the branch name.
  const index = refName.lastIndexOf("/");
  if (index == -1) {
    return refName;
  }
  return refName.substring(index + 1);
}
