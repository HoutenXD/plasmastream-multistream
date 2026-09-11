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

The trade is that every destination gets identical settings. That is the right
default, and it is why the outputs start and stop with your main stream rather
than having controls of their own.

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

`buildspec.json` pins OBS 31.1.1 rather than the newest release. Building against
the older API means the plugin also loads on OBS 31, and nothing here uses
anything added since.

## Where this is published

https://github.com/HoutenXD/plasmastream-multistream

Development happens in the private PlasmaStream monorepo under
`plugin/obs-multistream`, and that directory is exported to the public repo with
`git subtree`. The public repo is the export, not the working copy: commits made
there directly will be overwritten by the next export.

**Re-export before releasing any binary, not after.** The GPL entitles whoever
receives a build to the source *that build came from*, so a public repo one
commit behind a published binary is a licence problem rather than an untidiness.
Run this from the monorepo root:

```
git subtree split --prefix=plugin/obs-multistream -b plugin-public --rejoin
git push https://github.com/HoutenXD/plasmastream-multistream.git plugin-public:main
```

Nothing else from the monorepo travels with it: `subtree split` rebuilds a
history containing only commits that touched this directory, and only the files
inside it.

## Licence

GPL-2.0-or-later, and not by choice: this links against libobs, which is GPL-2.0,
so the plugin is a derivative work and has to be. Anyone who receives a binary is
entitled to this source.
