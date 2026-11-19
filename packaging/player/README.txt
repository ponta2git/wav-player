WAV Player

Playback PC requirements

- Windows 11 version 25H2 or later (x64)
- A CPU with AVX2, BMI1, BMI2, LZCNT, MOVBE, FMA and F16C
- The x64 ASIO driver from your audio device manufacturer

Play your first track

1. Copy the entire player folder to a writable location on a local drive.
2. Open setup.cmd. Enter the number shown for your ASIO driver, then enter
   the full path to your local WAV folder (for example, C:\Music).
   Setup includes WAV files in subfolders.
3. When the player opens, check Device, Output and the track list. Set the
   volume on your audio device, select a track and click Play.

Next time, open wavplay.exe directly.

Update the playlist

Setup normally keeps your existing playlist.txt. To scan for tracks again,
close the player and run this command from a terminal in the player folder.

    .\setup.cmd -UpdatePlaylist

Enter the WAV folder you want to use. Setup saves the previous playlist.txt
as a .bak file in the player folder. Add -NoLaunch to the command to keep
the player closed after setup.

Change the device or output settings

Setup keeps your existing settings.ini. To change devices or adjust playback
settings, close the player, edit settings.ini and reopen wavplay.exe.
For example, set OutputPair=3 to use outputs 3 and 4. The default is 1
for outputs 1 and 2. See settings.example.ini for all device settings.

BufferSize=0 uses the driver's preferred buffer size. Some drivers keep a
custom size after you close the player, which can affect other audio apps.
