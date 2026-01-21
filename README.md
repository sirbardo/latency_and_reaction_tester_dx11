This repo contains two (mostly vibecoded) apps:

# Latency Tester (main.cpp)

Goal: to have a program that can be used together with an LDAT (or high fps camera) to measure click-to-photon (or motion-to-photon, if one has a sufficiently sophisticated measuring device) in a way that minimizes any app overhead.

Main features:
- Fullscreen Exclusive
- Flashes white on input (depending on configuration)
- Also doubles as a mouse hz tester
- Runs at 10k+ fps on most machines
- Can show a log of inputs including mouse deltas for motion testing

# Reaction Time Tester (reaction.cpp)

Goal: to have a program that can be used to test flash-to-click VRT in humans in a way that minimizes any overhead given by the app itself, and a way to measure sound reaction time in humans on Windows in a way that minimizes the (still rather large) overhead due to Windows audio API's.

Main features:
- Fullscreen Exclusive (basically shares the same underlying code for the rendering portion)
- Typical flash-to-click visual reaction time testing
- WASAPI based (as efficient as I could make it) sound testing (can probably be optimized further)



