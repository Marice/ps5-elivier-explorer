# Validating on the console

Pictures rendered on a PC prove a design looks right. Only the console proves
it runs: that the shaders compile there, the frame rate holds, the sound
plays and the app closes cleanly. The app has a self-driving run mode for
exactly that, so a validation takes one launch and no controller.

## The tour run

Two facts about a title's sandbox shape the mechanism:

- It has no `/data`. The app's output (log, pictures, report) goes to its own
  storage, `/download0/hui/dev`, which exists only while the title runs. A PC
  can *read* it over FTP as `/mnt/sandbox/<TITLE_ID>_000/download0/hui/dev`,
  but cannot write there.
- The install folder is writable from a PC and the app sees it as `/app0`.
  So requests travel that way: `/data/homebrew/<TITLE_ID>/dev/request.txt`
  holds one line, `tour <design id|all> <token>` or `quit - <token>`. The app
  honours a request once per token (it remembers the last one), so a request
  left in place does not make every later launch a tour.

When the app finds a tour request it has not honoured yet, it:

1. shows every design in turn and replays the inputs its `tour()` describes;
2. saves a small picture (480 x 270) of each tour state to
   `dev/tour/NN-<id>[-<state>].bmp`;
3. measures every presented frame per design;
4. writes `dev/tour/report.txt` and logs the same lines;
5. stays up for four more minutes, because closing unmounts the storage and
   the evidence with it;
6. asks the system to close it (`sceSystemServiceLoadExec("exit", NULL)`).

`dev/app.log` is the app's log. Put the request in place **before** launching:
the tool does not write into the install folder of a running title.

The design id `all` tours every design; one id (`aurora`) tours only that one.

The report has one line per design:

```
tour 01 aurora       frames=412 avg_ms=16.67 worst_ms=16.92 draws=14 shapes=373
```

`avg_ms` near 16.67 with a `worst_ms` under about 20 means the design held
60 frames per second through its whole tour, including dialogs and blur.

## Running it

`tools/console-tour.py` does the whole cycle from a PC on the same network.
It needs FTP (2121), the kernel log (3232), an ELF loader (9021) and
[ShadowMount Plus](https://github.com/drakmor/ShadowMountPlus) on the
console, and the launch helper from
[ps5-homebrew-dev-protocol](https://github.com/blackbearreloaded/ps5-homebrew-dev-protocol).

```bash
make                                            # build dist/<TITLE_ID>
PS5_HOST=<console address> tools/console-tour.py results/run-1
```

It uploads the build (every file read back and compared by hash), writes the
trigger, waits until the console has registered the title, records the kernel
log, launches the title, waits for the app's own exit, downloads the report,
the pictures and `app.log`, and checks the log for errors. It never kills the
app and never retries.

A title copied to `/data/homebrew` for the first time is not known to the
console yet. ShadowMount Plus registers it once the folder has stopped
changing (about ten seconds), which creates `/user/app/<TITLE_ID>/mount.lnk`;
the tool waits for that file. Launching earlier only shows the system's
"title not found" screen: nothing starts and nothing is harmed, but the run
is wasted.

What to look at afterwards, in `results/run-1/`:

| File | Check |
| --- | --- |
| `report.txt` | Every design present; `avg_ms` about 16.67; no outlier `worst_ms` |
| `tour/*.bmp` | Compare with `build/snapshots/*.png` from the PC: same layout, colours and text |
| `app.log` | `gl 4.6`, `sounds files=67 rejected=0`, no `fatal`, `shader ... failed` or `rejected` lines; `tour finished`; `quit requested` |
| `klog.txt` | The title's start and its clean exit; no crash report |
| `shadowmount.txt` | The title's lines from the console's lifecycle log: registered, started, released |

## Probes: questions put to the console before a feature is built

A probe is a small measurement that answers one open question of the video work
(`docs/decision-log.md`, D41 and D44). A probe launch replaces the app: no
shell, no music, no controller. The request is `probe <name> <token>` in the
same `request.txt` the tour uses, the answers go to `dev/probe/report.txt` and
to the log line by line as they come, and the app closes itself two minutes
later (a `quit` request ends the wait early).

```bash
make                                                   # build dist/<TITLE_ID>
PS5_HOST=<console address> tools/console-probe.py results/probe-1 safe
```

| Name | What it asks | What it changes |
| --- | --- | --- |
| `memory` | How much direct memory can be had in blocks of 16 MiB up to 1 GiB (type 12) and 256 MiB (type 11), how fast it is written and read, and what flexible memory is left | Allocates and releases it again |
| `upload` | What it costs, in ms, to upload a 1080p and a 4K frame (8-bit NV12 and 10-bit) to textures, directly and through a buffer | Creates and deletes textures |
| `egl` | Which EGL extensions and window-surface formats the driver offers (10-bit, float, HDR colour spaces) | Nothing |
| `decoder-load` | Whether `libSceVideodec2` can be loaded from this title and its nine functions looked up. No decoder function is called | Loads a system module |
| `safe` | `memory+upload+egl+decoder-load` | As above |
| `hdr:pq10`, `hdr:hlg10`, `hdr:fp16` | Does the driver accept a 10-bit or half-float surface in an HDR colour space? Eight brightness steps are shown for 12 s | Closes the display and opens another surface. Runs alone |

Rules that follow from what is not known:

- **`safe` first, and read the report before anything else.** The `egl` lines say
  which HDR extensions the driver advertises and which configs exist; an HDR
  attempt is only worth a run if they are there.
- **One HDR attempt per launch, and someone must look at the TV.** The probe
  cannot see the screen. It reports what EGL said (`open`, `egl_error`,
  `colour_space_accepted`); whether the TV switched to HDR and whether the steps
  to the right are brighter is for the person in front of it, and is *not
  verified* until that person says so.
- **`decoder-load` stops at looking.** The decoder's structs are known only from
  other people's headers; a wrong layout could corrupt memory. If the functions
  are found, a later probe may call the two read-only query functions.
- **A failed allocation is an answer.** The ladder stops at the first block that
  does not fit; the report says which. Only `fatal` lines in the log or a crash in
  the kernel log are problems.
- The request is marked as honoured *before* the probes run, so a probe that
  crashes the app is not run again by the next launch.

The report has one line per answer, in a fixed shape:

```
probe memory t12_256mib ok write=2105 MiB/s read_and_check=3310 MiB/s
probe upload nv12_4k_buffer calls avg=1.10 p95=1.60 ms; done avg=4.20 p95=5.10 max=7.30 ms (n=60)
probe egl has_EGL_EXT_gl_colorspace_bt2020_pq=no
```

## Rules for console runs

These come from hard experience with shared development consoles:

- **Finish on the PC first.** Tests and snapshots must pass before a console
  is involved.
- **One run at a time, a handful per session.** Long unattended series of
  launches are how consoles get lost. Check that the console still answers
  between runs and stop at the first anomaly.
- **Let the app end itself.** The tour exits through the system. Do not send
  a kill to a title that is still rendering.
- **Take the console's lock** if it is shared, and release only your own.
- **Verify what you uploaded** after a pause: files written just before an
  unclean shutdown can come back empty.
- **Never touch the console's settings**, and never run anything that was not
  declared for the test.

## What a PC cannot tell you

State these as "not verified" until someone has checked them by hand:

- how the sounds actually sound through a TV, and their balance against music;
- how the controller rumble feels, and the light bar colour;
- how focus movement feels on a real stick (dead zone, repeat rate);
- text legibility from the sofa on the actual TV.
