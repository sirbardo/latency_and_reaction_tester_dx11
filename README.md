This repo contains two (mostly vibecoded) apps:

# Latency Tester (main.cpp)

Main features:
- Fullscreen Exclusive
- Flashes white on input (depending on configuration)
- Also doubles as a mouse hz tester
- Runs at 10k+ fps on most machines
- Can show a log of inputs including mouse deltas for motion testing

# Reaction Time Tester (reaction.cpp)

Main features
- Fullscreen Exclusive (basically shares the same underlying code for the rendering portion)
- Typical flash-to-click visual reaction time testing
- WASAPI based (as efficient as I could make it) sound testing (can probably be optimized further)



