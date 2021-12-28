
const {
  getLatestReplayRevision,
  sendBuildTestRequest,
  spawnChecked,
  newTask,
} = require("../utils");

const branchName = process.env.GITHUB_REF_NAME;
console.log("BranchName", branchName);

if (branchName.includes("webreplay-release")) {
  console.error("Use build/test action for release branch");
  process.exit(1);
}

const replayRevision = getLatestReplayRevision();

const driverRevision = process.env.INPUT_DRIVER_REVISION;
console.log("DriverRevision", driverRevision);

sendBuildTestRequest({
  name: `Gecko Build/Test Branch ${branchName} ${replayRevision}${driverRevision ? " driver " + driverRevision : ""}`,
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
      driverRevision,
    },
    platform
  );

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

  return [buildReplayTask, testReplayTask];
}
