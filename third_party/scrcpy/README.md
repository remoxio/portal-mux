# scrcpy server

`scrcpy-server-v5.0.1` is the unmodified device-side server of [scrcpy](https://github.com/Genymobile/scrcpy)
v5.0.1 (Apache-2.0, see `LICENSE`), from the release assets:
https://github.com/Genymobile/scrcpy/releases/download/v5.0.1/scrcpy-server-v5.0.1

SHA-256: `764eb6f79811d5211fe9df341120882ba9994c7a61b897d7bf3fb662e53bc536` (checked by CMake)

Portal Mux pushes it to the device over adb and speaks its protocol (`src/android/`). The server checks
that the client asks for exactly its version, so updating means replacing this file, the version and
hash in CMakeLists.txt, and checking the protocol in scrcpy's `app/src/{server,demuxer,control_msg,device_msg}.c`.
Distributions that build the server from source can point `PORTAL_MUX_SCRCPY_SERVER` at their own file.
