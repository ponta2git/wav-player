WAV Player — locally built player folder

Windows 11 25H2 or later, x64, AVX2 and the required associated CPU features.
Install the x64 ASIO driver supplied by your device manufacturer.
Visual Studio and the ASIO SDK are needed only on the build PC.

1. Copy this whole folder to a writable local folder on your playback PC.
2. Run setup.cmd. Choose the ASIO driver number and a local WAV folder.
3. Check Device and the track list, then press Play. Playback never starts automatically.

Existing settings.ini and playlist.txt are kept when setup runs again.
To change device or output, close the player and edit settings.ini; see settings.example.ini.
OutputPair=1 selects outputs 1/2. BufferSize=0 uses the driver's current preference.
StartSilence=100 inserts 100 ms before playback starts (rounded to whole buffers).
The player does not restore the driver's preferred buffer size on exit.

To rebuild the playlist, close the player and run:
  setup.cmd -UpdatePlaylist
The old playlist is kept as a .bak file. WAV subfolders are included.
Only local WAV files are supported. The player checks their formats on launch.
To configure without opening the player, add -NoLaunch.

This folder was built locally for use on your own PCs. The project publishes source only.
No SDK, driver, audio files or build tools are included. GPLv3 text: LICENSE.
