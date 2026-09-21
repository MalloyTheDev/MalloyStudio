# Developing MalloyStudio

This guide covers the toolchain, building, running the tests, the checks every change must
pass before it is committed, the conventions the code follows, and the problems you are most
likely to hit. For what the application does, see the [README](../README.md); for how it is
built internally, see [ARCHITECTURE.md](ARCHITECTURE.md).

---

## Toolchain

MalloyStudio is developed and tested with MSYS2's MinGW-w64 toolchain. Other toolchains
(MSVC, clang-cl) are not tested.

| Tool | Minimum | Tested with | Source |
|---|---|---|---|
| Windows | 10 1903 | Windows 11 | |
| MSYS2 MinGW-w64 GCC | C++20 support | 15.2 | `mingw-w64-x86_64-gcc` |
| Qt 6: Core, Gui, Widgets, Network, Test, Svg | 6.5 | 6.10.1 | `mingw-w64-x86_64-qt6-base`, `mingw-w64-x86_64-qt6-svg` |
| windeployqt6 (for deployment only) | | 6.10.1 | `mingw-w64-x86_64-qt6-tools` |
| CMake | 3.21 | 4.2 | cmake.org, or `mingw-w64-x86_64-cmake` |
| Ninja | | 1.13 | `mingw-w64-x86_64-ninja`, or any Ninja on `PATH` |
| ffmpeg and ffprobe | | 8.1.1 | Any Windows build on `PATH` (for example `winget install Gyan.FFmpeg`) |

Install the MSYS2 packages from an MSYS2 shell:

```sh
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-qt6-base mingw-w64-x86_64-qt6-svg \
          mingw-w64-x86_64-qt6-tools mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja
```

`CMakeLists.txt` finds Qt at `C:\msys64\mingw64` when neither `Qt6_DIR` nor
`CMAKE_PREFIX_PATH` is set. For a Qt installed elsewhere, set `CMAKE_PREFIX_PATH` to its
prefix before configuring.

ffmpeg and ffprobe are not linked: the application runs them as child processes and finds them
on `PATH`. Package-manager installs (Chocolatey, Scoop, winget) often put a small launcher on
`PATH` that starts the real program as its child; the code handles that (see `ProcessTree` in
[ARCHITECTURE.md](ARCHITECTURE.md)).

---

## Building

### With build.ps1

From PowerShell in the repository root:

| Command | Effect |
|---|---|
| `.\build.ps1 -NoLaunch` | Configure `build\` on the first run (Ninja, GCC, Debug), then build |
| `.\build.ps1 -RunTests` | Build, then run the test suite with `ctest --output-on-failure` |
| `.\build.ps1 -Deploy` | Build, run `windeployqt6 --debug --compiler-runtime`, copy every other MinGW DLL the binaries import, then launch |
| `.\build.ps1` | Build and launch |

The script puts `C:\msys64\mingw64\bin` first on `PATH` for its own process. `-Deploy` and
`-NoLaunch` can be combined.

### With CMake directly

From any shell, with the MinGW toolchain first on `PATH` (in Git Bash:
`export PATH="/c/msys64/mingw64/bin:$PATH"`):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Pass `-j N` to `cmake --build` to limit parallel jobs. Close any running `MalloyStudio.exe`
first: while it runs, Windows keeps the executable locked and the link step fails.

### What the build produces

| Target | What it is |
|---|---|
| `MalloyStudio.exe` | The application (a GUI program with no console) |
| `MalloyStudioTests.exe` | The main QtTest suite: model, capture, audio, encoding, persistence and UI rules |
| `MalloyOutputSettingsTests.exe` | QtTest suite for output settings and their persistence |
| `MalloyCaptureHandoffTests.exe` | QtTest suite for the bounded frame handoff between capture and composition |
| `MalloyPipeIoTests.exe` | Plain test program for cancellable named pipe I/O |
| `MalloyCaptureLifetimeTests.exe` | Plain test program for the capture callback gate's lifetime and serialisation |

The application version comes from `project(MalloyStudio VERSION ...)` in `CMakeLists.txt`
and is compiled into `src/main.cpp` only.

### Running from the build folder

`MalloyStudio.exe` needs the Qt and MinGW runtime DLLs. Either run `.\build.ps1 -Deploy`
once, which copies them next to the executable, or start it from a shell whose `PATH` begins
with `C:\msys64\mingw64\bin`.

---

## Tests

Run the whole suite from the build folder:

```sh
cd build
ctest --output-on-failure
```

`MalloyStudioTests` holds about 200 test functions and takes one to two minutes; the other
four finish in about a second. To run chosen test functions, call the QtTest executable
directly. Writing the log to a file with `-o` is the reliable way to capture it from any shell:

```sh
./build/MalloyStudioTests.exe aRecordingKeepsItsColours soundAndPictureStayTogether -o out.txt,txt
./build/MalloyStudioTests.exe -functions      # list every test function
```

### What the tests need and touch

- **ffmpeg and ffprobe on `PATH`** for the tests that run real encodes, recordings and renders.
  Those tests skip themselves when ffmpeg is missing.
- **The default playback device.** Constructing an `AudioController` opens WASAPI loopback
  capture of it ([#98](https://github.com/MalloyTheDev/MalloyStudio/issues/98)). Other devices
  (cameras, microphones, displays, windows) are replaced by fakes through the factory seams
  (`setWorkerFactoryForTesting`, `setCameraSessionFactoryForTesting`,
  `setWindowSessionFactoryForTesting`, and others).
- **Settings** under `HKCU\Software\MalloyStudioTests\ModelTests` for the main suite, isolated
  from the application's own; the output settings suite uses temporary INI files.
- **A test credential** named `MalloyStudioTests_TwitchTokens` in Windows Credential Manager,
  removed by the tests that create it. The real stream key and Twitch tokens are never touched.
- **Native windows that are never shown**, for window capture and widget tests. Do not set
  `QT_QPA_PLATFORM=offscreen`: the MSYS2 Qt build ships only the `windows` platform plugin, and
  forcing another makes every test program fail to start with a dialog.
- **Temporary folders** for every file a test writes.

Two suites running at the same time on one machine can interfere, because they share the test
settings and the test credential. Some recording tests measure timing (tick rate, audio and
video sync, stop behaviour) and can fail on a heavily loaded machine; run a failing one again on
its own before drawing conclusions.

### Writing tests

- Every behaviour change comes with a test in the suite that covers that area, next to the
  existing tests for it.
- A test must fail without the change it covers. Check this by reverting the change, running the
  test, and seeing it fail, before restoring the change.
- Use the existing seams rather than real devices, and temporary folders rather than the user's
  folders. End any child process a test starts; for ffmpeg and ffprobe use `ProcessTree::kill`,
  which also ends the child of a package-manager launcher.
- Prefer measured assertions with a bound derived from a measurement (and say what was measured
  in a comment) over sleeps and guesses.

---

## The validation gate

Continuous integration is not the gate for this repository; the local checks are. Before
committing, with no `MalloyStudio.exe` running:

1. `cmake --build build` succeeds with no errors.
2. `cd build && ctest --output-on-failure` passes, judged by its exit code, not by reading the
   tail of its output.
3. The diff adds no em dash character (U+2014). Many older files still contain some, so the
   check is on added lines only:

   ```sh
   git diff HEAD | grep -nP '^\+.*\xe2\x80\x94'     # must print nothing
   ```

   New files that are not yet tracked need the same check.
4. No process started by the build or the tests is left running
   (`tasklist | grep -iE "ffmpeg|ffprobe|MalloyStudio"`).

---

## Conventions

### Commits and branches

- Work on `master`, the trunk. Commit one coherent change at a time, with its tests.
- Commit subjects are short and imperative and describe the engineering change ("Tick the
  video clock against absolute deadlines"). The body says what was wrong, what changed and why,
  and what the test demonstrates.
- Findings that do not belong in the current change become GitHub issues with enough detail for
  someone else to pick them up: the problem, the component, how to reproduce it, expected and
  actual behaviour, the impact, a direction and acceptance criteria. A code comment may point
  to an issue; it is not a substitute for one.

### Code

- C++20, Qt 6, Windows APIs directly where Qt has no equivalent. No new dependency without a
  clear need that Qt, the standard library or Windows cannot meet.
- Comments explain why: invariants, protocol requirements, platform behaviour and the failure a
  piece of code prevents. They do not restate the code.
- Treat everything from outside the process as untrusted: project files, settings, device
  formats, ffprobe output and network responses are validated and bounded before use.
- Anything that can block (ffmpeg, device calls, file system calls on paths that may be on a
  network share) stays off the GUI thread or is bounded by a timeout.
- Every process the application starts ends on its own or is ended, together with any process
  it started.
- Use plain ASCII punctuation in source, comments and documents; in particular, never the em
  dash character.

### Documentation

Update the documents whose subject a change affects: behaviour visible to users in the README,
internals in [ARCHITECTURE.md](ARCHITECTURE.md), the file format in
[PROJECT_FORMAT.md](PROJECT_FORMAT.md) and a notable change in [CHANGELOG.md](CHANGELOG.md).
Keep implemented, partial and planned behaviour clearly apart; never describe planned work as
existing.

---

## Diagnostics

- **Frame timing profile.** Set `MALLOY_PROFILE_FRAMES=1` for one run (or the setting
  `profile/frames` to `true` for every run). Each second the log gets a per-stage timing report:
  capture readback, handoff to the GUI thread, composition, conversion and pipe writes, among
  others.
- **Run summary.** Every recording and stream logs one `capture stages:` line when it stops,
  with the frames the capture side produced and dropped, the frames composed, accepted and
  written to ffmpeg, frames dropped at the encoder input, repeats, late clock ticks and dropped
  audio. The gaps between those numbers locate a problem. The fields are described in
  [ARCHITECTURE.md](ARCHITECTURE.md).
- **ffmpeg errors.** An error dialog for a failed recording or stream shows ffmpeg's stderr
  output under its details.
- **Logs.** The application writes its log through Qt's message handler (`qInfo`, `qWarning`).
  It is a GUI program with no console; attach a debugger or a debug output viewer to read the log
  of a running instance.

---

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| "This application failed to start because no Qt platform plugin could be initialized" | The program could not find Qt's `windows` platform plugin. Run `.\build.ps1 -Deploy`, or start it from a shell with `C:\msys64\mingw64\bin` first on `PATH`. Do not set `QT_QPA_PLATFORM` to a plugin the build does not ship. |
| "Entry Point Not Found" naming a symbol, at startup | A different MinGW runtime was found first on `PATH` (Git for Windows ships one). Deploy with `-Deploy`, or put `C:\msys64\mingw64\bin` first on `PATH`. |
| The same error after updating MSYS2 with `pacman -Syu` | DLLs copied by an earlier `-Deploy` are out of date, and the deploy step does not replace DLLs that are already present ([#59](https://github.com/MalloyTheDev/MalloyStudio/issues/59)). Delete the copied DLLs from `build\` and deploy again. |
| The link step fails with "Permission denied" on `MalloyStudio.exe` | The application is still running. Close it (or `taskkill /IM MalloyStudio.exe /F`) and build again. |
| Record and Go Live are disabled | ffmpeg is not on `PATH` for the process. Check with `where ffmpeg` in the same kind of shell the application is started from. |
| A hardware encoder is missing from the encoder list | Hardware encoders are listed only after their trial encode succeeds on this machine, a few seconds after startup. An encoder that fails the trial (no such GPU, an old driver) is not offered. |
| Tests fail only when another test run or a heavy build is active | See [What the tests need and touch](#what-the-tests-need-and-touch): suites share test settings, and some tests measure timing. Run the failing test again on its own. |
