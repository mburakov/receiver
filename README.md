# Receiver

This is a lightweight streaming client that works together with [streamer](https://burakov.eu/streamer.git) server. It receives encoded HEVC bitstream over tcp connection, decodes it using Intel Media SDK, and renders the resulting buffers using Wayland. Everything is done hardware-accelerated and zero-copy. Receiver listens for input events from a Wayland compositor, converts those to UHID messages, and sends those to streamer server over the same tcp connection.

## Building on Linux

Receiver depends on following libraries:
* libva
* libva-drm
* mfx
* wayland-client

Once you have these installed, just
```
make
```

## Building anywhere else

I don't care about any other platforms except Linux, so you are on your own. Moreover, I don't really expect it would work anywhere else.

## Running

There are couple of things receiver implies, i.e. that your system is supported by Intel Media SDK. Wayland compositor is expected to support following protocols:
* linux-dmabuf-unstable-v1,
* pointer-constraints-unstable-v1,
* relative-pointer-unstable-v1,
* xdg-shell.

This is certainly the case with any recent Intel CPU and sway compositor. It would probably work with other wlroots-based compositors too. I never cared enough to check if Intel Media SDK works on AMD systems. In case it does, receiver would work on AMD hardware as well. Nvidia configurations are certainly not supported. Not just VA-API, but also linux-dmabuf-unstable-v1 Wayland protocol are not expected to work. So no Nvidia please.

Provide ip address and port number of listening [streamer](https://burakov/streamer.git) instance on the commandline:
```
./receiver 192.168.8.5:1337
```

## What about Steam Link?

For a long time I was suffering from various issues with Steam Link:
* client builtin into usual desktop steam client does not support hardware-accelerated video decoding,
* decicated steam link application is redistributed as a flatpak, and requires to install related bloatware,
* streaming occasionally crashes on server-side bringing down Steam together with streamed game,
* using a mouse wheel makes server-side Steam to crash immediately together with streamed game,
* only back screen is streamed unless server-side Steam is running in big picture mode,
* one of recent updates broke streaming entirely, producing black screen even when running server-side in big picture mode,
* many other minor issues...

## What about Sunshine/Moonlight?

I tried this one too. Issues there are not as severe as with Steam Link, but still:
* relies on Qt, boost and other questionable technologies,
* does not properly propagate mouse wheel events making it pretty much unusable,
* video quality occasionally degrades into a blurry brownish mess and never recovers,
* extremely thin Moonlight video-related configuration options are seem to be ignored entirely by Sunshine,
* Sunshine crashed on me when I attempted to stream a game running with Proton.

## But there's no audio

This is correct, audio streaming is not supported as of today. Actually pulseaudio has quite a decent implementation of forwarding audio streams over network. When using module-native-protocol-tcp combined with module-tunnel-sink I only get minor distortions when connecting bluetooth headset. Connecting wired headphones produces seamless experience. That said, I might consider adding audio streaming support to the project.

## Fancy features support status

There are no fancy features in streamer. There's no bitrate control - VA-API configuration selects constant image quality over constant bitrate. There's no frame pacing - because I personally consider it useless for low-latency realtime streaming. There's no network discovery. There's no automatic reconnection. There's no codec selection. There's no fancy configuration interface. There are no options at all. I might consider implementing some of that in the future - or might not, because it works perfectly fine for my use-case in its current state.

At the same time, it addresses all of the issues listed above for Steam Link and Sunshine/Moonlight. No issues with controls, no issue with video quality, no issues with screen capturing. On top of that instant startup and shutdown both on server- and client-side.

## Where is toolbox and pui?

Note, that I don't use github for actual development anymore - it's just a mirror these days. Instead, I self-host git repos on https://burakov.eu. Read-only access is provided via cgit, i.e.: https://burakov.eu/receiver.git. Same stands for toolbox and pui (primitive ui) submodules, which are fetched via https using git commandline. You can as well access the code of toolbox and pui directly using your browser: https://burakov.eu/toolbox.git, https://burakov.eu/pui.git.
