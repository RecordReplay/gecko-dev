This branch contains modifications to the mozilla:release branch for compiling
the Record Replay gecko based browser.

### Getting started:

**MacOS**

If you are using Apple Silicone you should start by making sure you have a Rosetta terminal:

* https://apple.stackexchange.com/questions/428768/on-apple-m1-with-rosetta-how-to-open-entire-terminal-iterm-in-x86-64-architec

Then you should basically be able to follow the rest of the steps normally (with a couple of caveats called out down below - read carefully).

1. Make sure that you are using Python v2.7
2. `cp mozconfig.macsample mozconfig`
3. Download `MacOSX11.1.sdk.tar.xz` from https://github.com/phracker/MacOSX-SDKs/releases
4. Untar `MacOSX11.1.sdk.tar.xz` in the repo root to create a `MacOSX11.1.sdk` directory
5. Run `node build`
   * On Apple Silicon, you many need to run `RUSTC_BOOTSTRAP=qcms node build` to build successfully.
6. Run `./mach run`

**MacOS with Apple Silicone**


**Linux**

1. `cp mozconfig.linuxsample mozconfig`
2. run `./mach bootstrap` and select (2) Firefox Desktop
3. run `node build`
4. run `./mach run`

### Troubleshooting Tips

* If you change your PATH to point to a different version of say Python or Rust you need to rerun `./mach bootstrap` to get the build system to pick up the change.

* If you are seeing this error:

```
ERROR!!!!!! Could not find artifacts for a toolchain build named macosx64-dump-syms
``` 
Try cloning the latest mozilla-central (context: https://discord.com/channels/779097926135054346/801228428115312671/938567563644915713):

* Make sure you have mercurial installed (`brew install hg`)
* Clone mozilla central to sibling directory to this one (`cd .. && hg clone https://hg.mozilla.org/mozilla-central/`) 
* Go into the freshly cloned mozilla-central repo and run `./mach bootstrap` within it and select `(2) Firefox Desktop` when prompted. Come back to this repo and try building again.
* In some cases, even if `./mach bootstrap` fails with the above error, the build step might still work, so you can also try building without necessarily getting `./mach bootstrap` to complete everything successfully.

You can also find some conversation of these steps at https://github.com/RecordReplay/gecko-dev/issues/745

### Merging from upstream

1. Checkout the `release` branch, pull from upstream `release` branch:

```
git checkout release
git pull https://github.com/mozilla/gecko-dev.git release
```

2. Switch to a new branch, merge from the `release` branch.

```
git checkout webreplay-release
git checkout -b replay-merge
git merge release
```

3. Fix merge conflicts.
4. Fix build breaks.
5. Make sure the output binary is `replay` and not `firefox`.
6. Get e2e tests etc. to pass.
7. At this point it is reasonably safe to merge into the `webreplay-release` branch.

```
git checkout webreplay-release
git merge replay-merge
git push
```

8. Update User Agent version reported by `CurrentFirefoxVersion()` in `toolkit/recordreplay/ProcessRecordReplay.cpp`
9. Make sure automatic updates work with the new browser. Run the build/test action on the merge branch, delete the `noupdate` file for the build in S3, then launch the browser, open "About Replay" and see if it updates.

Tips for debugging:

* Look at the update server logs to make sure requests are being processed correctly.
* Set the `app.update.log` browser config when running, and then check console output.
* If there is a line like `*** AUS:SVC readStatusFile - status: failed: 23, path: /path/to/update.status`, this is produced by the C++ updater which can be found in `UpdateThreadFunc` in `updater.cpp`. Building a local browser with instrumentation is likely needed to investigate.

10. Run live test harness and make sure crash rate is acceptable.


### Miscelleneous

Speeding up oh-my-zsh

```
git config --add oh-my-zsh.hide-status 1
git config --add oh-my-zsh.hide-dirty 1
```
