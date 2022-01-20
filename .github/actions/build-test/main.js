
const {
  getLatestReplayRevision,
  getLatestPlaywrightRevision,
  sendBuildTestRequest,
  spawnChecked,
  newTask,
} = require("../utils");

const replayRevision = getLatestReplayRevision();
const unmergedPlaywrightRevision = getLatestPlaywrightRevision();

const mergePlaywrightTask = newTask(
  `Merge into playwright branch`,
  {
    kind: "MergeBranches",
    mergeKind: "geckoPlaywright",
    sourceRevision: replayRevision,
    targetRevision: unmergedPlaywrightRevision,
    updateRevisionTasks: [],
  },
  "macOS"
);

sendBuildTestRequest({
  name: `Gecko Build/Test ${replayRevision}`,
  tasks: [
    mergePlaywrightTask,
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
    },
    platform
  );

  const testReplayTask = newTask(
    `Run Tests ${platform}`,
    {
      kind: "StaticLiveTests",
      runtime: "gecko",
      revision: replayRevision,
    },
    platform,
    [buildReplayTask]
  );

  const tasks = [buildReplayTask, testReplayTask];

  // Playwright builds aren't yet available for windows.
  if (platform != "windows") {
    const buildPlaywrightTask = newTask(
      `Build Gecko/Playwright ${platform}`,
      {
        kind: "BuildRuntime",
        runtime: "geckoPlaywright",
        revision: "",
      },
      platform,
      [mergePlaywrightTask]
    );
    mergePlaywrightTask.updateRevisionTasks.push(buildPlaywrightTask.id);

    const testPlaywrightTask = newTask(
      `Test Gecko/Playwright ${platform}`,
      {
        kind: "PlaywrightLiveTests",
        runtime: "geckoPlaywright",
        revision: "",
      },
      platform,
      [buildPlaywrightTask]
    );
    mergePlaywrightTask.updateRevisionTasks.push(testPlaywrightTask.id);

    tasks.push(buildPlaywrightTask, testPlaywrightTask);
  }

  return tasks;
}
