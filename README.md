# PlasmaStream Multistream

An OBS plugin that sends one broadcast to as many places as you want. Your PC
talks to each platform directly, so nothing routes through a middleman.

It works with no PlasmaStream account. If you have one, it can pull your
destination list from it, which saves retyping server URLs on a second machine.

## How it works

When you press Start Streaming, the plugin borrows the encoder OBS is already
using and points extra RTMP outputs at it. That is what makes a second
destination cheap: the frames are compressed once, so the extra cost is upload
bandwidth rather than CPU.

Borrowing means identical settings, which is the right default and not always
the right answer. Each destination can instead be given its own encoder, with
its own bitrate and its own choice of encoder from whatever the machine has.
That costs CPU, and it buys the thing people actually run into: an upload that
cannot carry two copies of a 6000 kbps stream, but can carry one of those and
one at 2500.

### Sound per destination

Each destination hears the main stream's sound unless it is given tracks of its
own: any of OBS's six audio tracks, in the destination's **Sound** settings.
One track is sent as that track's mix. Two or more are added together into the
one sound the platform gets, so music on YouTube and not on Twitch is the music
on a track of its own, ticked for YouTube and not for Twitch. Which sources are
on which track is set in OBS under Edit, Advanced Audio Properties.

A destination with its own sound still shares the main stream's picture; only
the audio is encoded again, which costs next to nothing. The dialog says who is
on each track, and warns about a track with nothing on it and about a source on
two of the chosen tracks, which the mix would play twice as loud.

The tracks are mixed by the plugin, on OBS's own timestamps, so the sound stays
in step with the picture. They are never sent as separate audio tracks: RTMP
gives every platform one track to play, and most would drop the rest.

### Vertical

A destination can be sent a 9:16 frame instead. There is one vertical canvas,
shared by every destination pointed at it, and it exists whenever the plugin is
loaded rather than only while you are live, because a frame you cannot arrange
until you are broadcasting is not one you can arrange.

What goes on it is your wide program, placed as a block. Not necessarily
cropped: the block can be the whole frame, or a band across the top, middle or
bottom with the rest free for other things. That choice is remembered **per
program scene**, because a full-bleed crop suits gameplay and ruins a talking
head, and nobody wants to redo it every time they switch.

Around that block you can put your own sources. Your camera and your chat
overlay are already in your scene collection; adding them here borrows them onto
the tall frame so they can sit where a phone wants them instead of where a
monitor does. They stay exactly where they are in your wide scenes. A source is
held active while it is on the vertical frame, so a camera that appears in none
of your wide scenes still lights up for this one.

Everything on the frame can be dragged: click to select, drag to move, pull a
corner to resize. Transform and Properties open OBS's own dialogs rather than
copies of them.

Vertical always encodes separately: the main stream's encoder is tied to the
main canvas and cannot be told to produce a different picture.

### The Canvases dock

A second dock showing the wide frame and the tall one side by side, both live.
Its own dock rather than a tab, so it can go on a second monitor, and so
destination health does not disappear behind a tab at the moment you want it.

The vertical pane is the taller of the two on purpose. OBS already shows your
wide frame in its own preview; the vertical one is the reason to open this.

Both docks start hidden, as every OBS dock does. Tick them in the **Docks** menu
in the menu bar, which is its own top-level menu next to View rather than
something inside View.

### While you are live

The tick box beside each destination works mid-stream. Turning one off stops
that destination and leaves everything else running, which is what you want at
the moment a platform starts rejecting frames and you would rather not end the
broadcast to deal with it. Turning it back on starts it again.

The dock shows, per destination, the bitrate actually going out, how long it has
been connected, how many frames it has dropped, and how many times it has
reconnected. The bitrate is measured from bytes sent, not read back from the
setting, because the gap between the two is the whole question when something
starts dropping.

### Recording a different scene

OBS records what is on program, so today a stream scene carrying a chat box and
alerts and a clean scene for the video are a choice between two things. This
records the one you are not showing.

Press **Record a scene** in the dock, pick the scene, and it runs whenever you
go live. The scene you pick is held active while it records, which is what makes
its sources run even though nobody is watching it, and it is recorded at your
stream's resolution into the folder OBS already records to, with the filename
format you already set. It does not touch OBS's own Start Recording button;
both can run at the same time.

It encodes separately, for the same reason vertical does, so it costs one more
encode. Bitrate, encoder and container are all settable. mkv is the default
because it survives a crash with the footage intact. mp4 is OBS's Hybrid MP4,
which survives a crash too and opens straight in an editor. mov is there for
lossless audio in an editor.

#### Several audio tracks

Tick any of OBS's six audio tracks and each becomes its own track in the file,
named however you like, so your microphone, Discord and the game arrive in your
editor as separate tracks to cut and level on their own. The dialog shows who is
on each track, and warns about a ticked track with nothing on it.

Most people have never put a source on its own track, so **Give a source its own
track** does it for you: pick the sound and it is ticked onto a spare track in
OBS's Advanced Audio Properties and named after itself. It only ever adds a
track, never takes one away, so your stream sounds exactly as it did. It also
leaves Twitch's VOD track alone.

**Lossless audio** records uncompressed 24-bit sound instead of AAC, in mkv and
mov. Hybrid MP4 stays on AAC, because the lossless codecs it could carry are ones
most editors refuse inside an mp4.

### Chapter markers

While anything is recording, the moments worth finding again become chapters:
raids, subs and memberships, cheers of 100 bits or more, donations and Super
Chats, and the game changing. PlasmaStream's bot sees each one as it happens and
the plugin collects them, so this needs your PlasmaStream key.

They go into OBS's own recording and into the scene recording, two ways:

- A chapters file beside every recording, named like the video with
  `.chapters.txt` on the end, one line per moment: `12:34 Raid: Mr_Dino_Saur_, 42
  viewers`. It works with any format, and pastes straight into a YouTube
  description. Each line sits where the moment happened, with any pause in OBS's
  recording taken out.
- Chapters inside the file itself, when the file is a Hybrid MP4, the one format
  OBS can write chapters into. Players and some editors show them.

To mark a moment yourself: press **Mark** in the dock, set a key for
"PlasmaStream: add a chapter marker" in OBS Settings > Hotkeys, or have a
moderator type `!marker` in chat with what happened after it. **Markers...**
chooses which kinds of moment go in.

### Voice commands

With a PlasmaStream account, you can say your wake phrase and then one of your
phrases ("Jarvis, set my game to Hades") and PlasmaStream runs the command behind
it. The phrases and the wake phrase are set on your dashboard, under Voice
commands. Press **Voice commands** in the dock, tick the box and pick your
microphone.

Speech is turned into text on your computer, by an open model (Moonshine, run by
sherpa-onnx), and the wake phrase is matched there too. Only the words after it
are sent to PlasmaStream; everything else the microphone hears is dropped. Muting
the microphone in OBS stops it listening. It works whether or not you are live,
so phrases can be tried before a stream.

There is also a push-to-talk key, under OBS Settings, Hotkeys, "PlasmaStream:
hold to give a voice command". While it is held, what you say is sent as a
command without the wake phrase, and nothing is decoded at all while it is not.

Windows only for now. The runtime and the models add about 190 MB, and they live
in the plugin's data folder rather than beside its DLL, so another plugin that
ships its own ONNX Runtime cannot overwrite them. If one has already loaded its
copy, voice commands stay off and the dock says why, rather than risk crashing
OBS.

### Stream check

Three things that are usually only noticed afterwards, from a clip or from chat:
your microphone was muted the whole segment, nothing at all was going out, or the
stream was still running an hour after you raided out and left. Press **Stream
check** in the dock and switch on the ones you want. All three are off until you
ask for them, and each is its own switch.

They appear in the dock and nowhere else. Nothing is drawn on your stream,
nothing is sent to PlasmaStream, and none of it is recorded.

Each one waits until it is sure. A muted microphone is flagged after a minute and
a half, which is past coughing, taking a sip or turning to say something to the
room. Silence is flagged after a minute with nothing audible anywhere in your
mix, so game sound and music count and it only fires when a viewer really is
hearing nothing. Still live is flagged after half an hour with no sound and no
scene change, so streaming quietly never trips it, because any noise at all
starts the clock again.

The thresholds are deliberately generous. A warning that is wrong once is a
warning you stop reading, including on the night it is right.

### Twitch Enhanced Broadcasting

If your main stream is using it, OBS is producing a ladder of several encodings
rather than one. A destination set to share picks the largest of them, which is
the picture you think you are sending; taking whichever encoder happens to be
first would quietly relay Twitch's lowest rung to your other platform.

Twitch's own dual-format vertical output is part of that ladder and belongs to
OBS. This plugin does not touch it. If you want a portrait feed somewhere else,
that is what a vertical destination above is for.

### What this is not

The vertical frame holds your wide program as one picture, plus your own sources
over it. It does not have its own scene list, and a source inside your program
cannot be pulled out of it and rearranged on its own, because on this canvas the
whole wide composition is a single element.

If that is what you need, Aitum Vertical does it and is free.

There is also no replay buffer on either the vertical canvas or the scene
recording, and the only hotkeys are voice push-to-talk and adding a chapter
marker: everything else is driven from the docks.

## Updating

The plugin asks https://plasmastream.live/api/plugin/update, twenty seconds after
OBS starts and every six hours after that, whether there is a newer version than
the one running. The request carries nothing: no token, no channel, no machine
identifier. Everyone who asks gets the same answer, and the check can be switched
off in **Options**.

When there is one, the dock says so and offers a button.

- **Update now** downloads the new version and unpacks it beside the installed
  one, under `%LOCALAPPDATA%\PlasmaStream\update`. Nothing is replaced yet.
- **Install and restart OBS** hands the swap to a small script, asks OBS to close,
  and starts it again once the files are in place.
- Closing OBS normally does the same thing without the restart, so an update that
  was downloaded and forgotten about goes in the next time OBS shuts down.

The swap happens after OBS exits because it has to: OBS holds the plugin DLL open
for as long as it runs, so nothing can replace it while OBS is up. The script
waits for this OBS process to end, copies the unpacked folder over the installed
one with robocopy, and leaves a one line result the plugin reads at the next
start. An update that could not be copied says so in the dock rather than
silently not happening.

**Options** also has *fetch it in the background*, which does the download part on
its own so the only thing left is closing OBS. It is off by default.

What it will not do:

- Install anything that needs a password. The macOS package and the Linux .deb
  are the system's business, so on those platforms the dock offers the download
  page instead.
- Touch an install it does not recognize. It replaces a plugin living in the
  layout the Windows zip produces, `<plugins>\plasmastream-multistream\bin\64bit`,
  and offers the download page for anything else, which includes a build you
  placed by hand.

Three Windows archives are published per release: the full one, the same build
without the speech models, and the models on their own. An install whose models
already match the release takes the middle one, which is around twenty megabytes
rather than a hundred and forty. Which models an install has is written in
`data/voice/models/models.txt` by the build that put them there.

## Your stream keys stay on your computer

They are written to the plugin's own config file, next to your OBS profile, and
nowhere else. The PlasmaStream website stores server URLs and names; it
deliberately has no field for a stream key and no column to put one in, because
a stream key lets anybody broadcast as you.

Syncing therefore **merges** rather than replaces. Destinations you already have
keep their keys and take the server's name and address; new ones arrive without a
key and the dock says so until you add one.

You can delete everything the plugin knows by deleting its config directory. The
dock shows the path.

## Building

Needs CMake 3.28+, and on Windows Visual Studio 2022 with the C++ desktop
workload. Everything else, including OBS itself and Qt, is downloaded during
configure.

```
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```

On Windows, configure also downloads the voice command runtime and models
(sherpa-onnx, two Moonshine models, Silero VAD and their licenses, about 250 MB),
each pinned and checked by SHA-256; see `cmake/voice.cmake`. `-DENABLE_VOICE=OFF`
leaves them out, and the dock then says voice is not in the build.

macOS and Linux use their own presets, `macos` and `ubuntu-x86_64`. macOS needs
Xcode 16 or newer, because the OBS template refuses anything below the macOS
15.0 SDK.

Linux builds against a libobs it finds on the system rather than a downloaded
one, because a plugin has to link against the libobs the user actually runs:

```
sudo apt install libobs-dev libcurl4-openssl-dev qt6-base-dev qt6-base-private-dev ninja-build
```

On Ubuntu 24.04 that is OBS 30.0.2, which predates `obs_canvas_*`, so a build
there comes out **without vertical**. The `PLASMASTREAM_HAS_CANVAS` test in
`outputs.hpp` compiles the canvas out and the dock disables the tick box with a
note saying why. Building against OBS 31.1 or newer, from source or from a
distribution that carries it, gets vertical back.

The OBS PPA does not solve this: it publishes `obs-studio` and no `libobs-dev`,
so apt resolves the headers from the Ubuntu archive regardless.

CI builds all three on every push and attaches them to a draft release on a
version tag. See `.github/workflows/build.yaml`.

The result lands in `build_x64/rundir/RelWithDebInfo`. To install it for testing
on Windows, copy it into the plugin folder OBS actually scans:

```
%PROGRAMDATA%\obs-studio\plugins\plasmastream-multistream\bin\64bit\plasmastream-multistream.dll
%PROGRAMDATA%\obs-studio\plugins\plasmastream-multistream\data\locale\en-US.ini
```

**PROGRAMDATA, not APPDATA.** Every other platform puts user plugins in the user
config directory and Windows does not: `AddExtraModulePaths` in
`frontend/widgets/OBSBasic.cpp` calls `GetAppConfigPath` on macOS and Linux, and
`GetProgramDataPath` on Windows.

Getting it wrong produces no error of any kind. OBS never scans the directory, so
the log holds no failure and no mention of the plugin at all, which reads exactly
like a broken binary.

The folder name has to match the DLL name, because libobs substitutes it into
`plugins/%module%/bin/64bit` as it scans.

OBS loads plugins at startup, so restart it after copying.

The dock is off until you ask for it: tick PlasmaStream Multistream in the
**Docks** menu. That is its own top-level menu in the menu bar (`menuDocks` in
`frontend/forms/OBSBasic.ui`, added to the menubar beside File and Edit), not an
entry inside View, which is where everyone looks first.

`buildspec.json` pins OBS 31.1.1 rather than the newest release, so the plugin
loads on more than just the current one.

**31.1 is the floor for a build that has vertical.** `obs_canvas_*` arrived in
31.1 and they are ordinary imports, so a binary built with them does not load on
an older OBS at all. `PLASMASTREAM_HAS_CANVAS` is what keeps that from meaning
"no Linux build": compiled against older headers the plugin drops vertical and
works everywhere else. The Windows and macOS releases are built against 31.1.1
and therefore need 31.1; the `.deb` is built against 30 and therefore does not.

## Where this is published

https://github.com/HoutenXD/plasmastream-multistream

Development happens in the private PlasmaStream monorepo under
`plugin/obs-multistream`, and that directory is exported to the public repo with
`git subtree`. The public repo is the export, not the working copy: commits made
there directly will be overwritten by the next export.

**Re-export before releasing any binary, not after.** The GPL entitles whoever
receives a build to the source *that build came from*, so a public repo one
commit behind a published binary is a license problem rather than an untidiness.
Run this from the monorepo root:

```
git subtree split --prefix=plugin/obs-multistream -b plugin-public --rejoin
git push https://github.com/HoutenXD/plasmastream-multistream.git plugin-public:main
```

Nothing else from the monorepo travels with it: `subtree split` rebuilds a
history containing only commits that touched this directory, and only the files
inside it.

`subtree split` walks the whole history and has crashed partway through on a
large one. When it does, export the release commit on its own instead, which
produces the same tree:

```
git format-patch -1 <sha> --relative=plugin/obs-multistream --stdout > /tmp/plugin.patch
git -C <clone of the public repo> am /tmp/plugin.patch
```

Then prove the two agree before pushing, which should print nothing:

```
git diff --stat main:plugin/obs-multistream <public head>^{tree}
```

## License

GPL-2.0-or-later, and not by choice: this links against libobs, which is GPL-2.0,
so the plugin is a derivative work and has to be. Anyone who receives a binary is
entitled to this source.
