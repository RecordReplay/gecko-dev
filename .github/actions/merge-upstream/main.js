
const {
  getLatestReplayRevision,
  sendBuildTestRequest,
  newTask,
} = require("../utils");

const replayRevision = getLatestReplayRevision();

const mergeTask = newTask(
  `Merge from upstream ESR branch`,
  {
    kind: "MergeBranches",
    mergeKind: "geckoUpstreamESR",
    targetRevision: replayRevision,
    updateRevisionTasks: [],
  },
  "macOS"
);

sendBuildTestRequest({
  name: `Gecko Upstream Merge ${replayRevision}`,
  tasks: [mergeTask],
});
