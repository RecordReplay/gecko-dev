
const {
  getLatestReplayRevision,
  getLatestPlaywrightRevision,
  sendBuildTestRequest,
  spawnChecked,
  newTask,
} = require("../utils");

const replayRevision = process.env.INPUT_RUNTIME_REVISION || getLatestReplayRevision();
const playwrightRevision = process.env.INPUT_PLAYWRIGHT_REVISION || getLatestPlaywrightRevision();

sendBuildTestRequest({
  name: `Gecko Release ${replayRevision}`,
  tasks: [
    ...platformTasks("macOS"),
    ...platformTasks("linux"),
    ...platformTasks("windows"),
  ],
});

function platformTasks(platform) {
  const releaseReplayTask = newTask(
    `Release Gecko ${platform}`,
    {
      kind: "ReleaseRuntime",
      runtime: "gecko",
      revision: replayRevision,
      driverRevision: process.env.INPUT_DRIVER_REVISION,
    },
    platform
  );

  const tasks = [releaseReplayTask];

  // Playwright builds aren't yet available for windows.
  if (platform != "windows") {
    const releasePlaywrightTask = newTask(
      `Release Playwright ${platform}`,
      {
        kind: "ReleaseRuntime",
        runtime: "geckoPlaywright",
        revision: playwrightRevision,
      },
      platform
    );

    tasks.push(releasePlaywrightTask);
  }

  return tasks;
}
