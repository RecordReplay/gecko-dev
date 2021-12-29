
const {
  getLatestReplayRevision,
  sendBuildTestRequest,
  spawnChecked,
  newTask,
} = require("../utils");

const branchName = getBranchName(process.env.GITHUB_REF);
console.log("BranchName", branchName);

const isReleaseBranch = branchName.includes("webreplay-release");

// When we're on the release branch, the latest released browser will be used for
// idle testing, even if it's not the most recent revision in the branch.
const replayRevision = isReleaseBranch ? undefined : getLatestReplayRevision();

const driverRevision = process.env.INPUT_DRIVER_REVISION;
console.log("DriverRevision", driverRevision);

sendBuildTestRequest({
  name: `Gecko Set Idle Testing ${branchName} ${replayRevision || ""}${driverRevision ? " driver " + driverRevision : ""}`,
  tasks: [
    ...platformTasks("macOS"),
    ...platformTasks("linux"),
    ...platformTasks("windows"),
  ],
});

function platformTasks(platform) {
  const task = newTask(
    `Set Idle Testing ${platform}`,
    {
      kind: "SetIdleTestRevision",
      revision: replayRevision,
      driverRevision,
    },
    platform
  );

  return [task];
}

function getBranchName(refName) {
  // Strip everything after the last "/" from the ref to get the branch name.
  const index = refName.lastIndexOf("/");
  if (index == -1) {
    return refName;
  }
  return refName.substring(index + 1);
}
