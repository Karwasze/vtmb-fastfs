vtmb-fastfs 1.0 - faster level loading for Vampire: The Masquerade - Bloodlines
==============================================================================

A plugin for the Unofficial Patch's plugin loader. It removes most of the
waiting at level transitions and the slow fade-in after every load.

Measured on the pawn shop -> Santa Monica hub transition (same PC, same save):

                                   without      with vtmb-fastfs
    door -> player has control     3.9 s        about 1.2 s
    loading a save                 about 2.1 s  about 1.1 s
    loading screen fade after load about 3 s    none

The first visit to a map after installing is a little slower than later ones:
the plugin learns which textures and file searches each map needs.


REQUIREMENTS
------------
- Vampire: The Masquerade - Bloodlines (GOG or Steam), 1.2 game files
- The Unofficial Patch (with its plugin loader: the folder Bin\loader must exist)
- Windows 7 or later

Every fix checks the game code it changes and simply switches itself off if
the code is not exactly the version it was made for (other patches or mods
that replace the same files). The game then behaves as without the plugin.


INSTALL
-------
Copy the "Bin" folder from this archive into the game folder (the folder that
contains Vampire.exe), merging it with the existing Bin folder. That adds:

    Bin\loader\vtmb-fastfs.vtm    the plugin
    Bin\loader\vtmb-fastfs.ini    its settings

Optionally copy this README next to Vampire.exe.


UNINSTALL
---------
Delete Bin\loader\vtmb-fastfs.vtm and Bin\loader\vtmb-fastfs.ini.
You can also delete the cache files the plugin creates there:
vtmb-fastfs-find.cache and vtmb-fastfs-tex.cache.
To switch it off without deleting anything, set "Enable = 0" in the ini.


WHAT IT CHANGES
---------------
Each item can be switched off in Bin\loader\vtmb-fastfs.ini (1 = on, 0 = off).

Index At Startup   The file system looked for a loose copy of every asset in
                   folders where none exist (~29,000 failed file opens per
                   load). The game folder is indexed at start-up instead.
Cache Searches     Wildcard searches (footstep sounds etc.) scanned every VPK
                   archive on every transition. Results are cached and kept in
                   vtmb-fastfs-find.cache while the game files are unchanged.
Fast Connect       While a level connects: no fps_max frame cap and no
                   loading-screen redraws waiting for V-Sync. In game your own
                   fps_max applies as usual.
Fast Parse         The game DLL's number parser rescanned the whole entity text
                   for every number. Fixed; results are bit-identical.
Fast Console       The console recounted its whole history for every printed
                   line, so loads got slower the longer you played.
Fast Model Lookup  Caches model lookups of the modelloader-fix plugin (only if
                   that plugin is installed).
Fast Inflate       Textures are decompressed with libdeflate instead of the
                   game's zlib from 2004 (several times faster, checksum
                   verified).
Prefetch Textures  Spare CPU cores decompress a level's textures ahead of the
                   game. A prefetched texture is only used if it matches the
                   game's data byte for byte. Lists are kept in
                   vtmb-fastfs-tex.cache.
Fast Bone Link     Bones were linked by comparing every name with every other;
                   now a hash lookup (same result).
Fast Anim Lookup   Animation lookups compared every sequence name; now a hash
                   lookup (same result).
Fast Symbols       Caches the file system's string-to-symbol lookups.
Skip Load Fade-In  The loading screen disappears as soon as the level is ready,
                   instead of staying for a second and fading out over two.
                   Also stops the extra fade from black on "start dark" maps.
                   Fades placed in levels by the designers (cutscenes, deaths,
                   scripted fades) are not affected.

Diagnostics (leave off for normal play):
Logging            Writes statistics to vtmb-fastfs.log in the game folder.
Log Console        Writes all engine messages with timestamps to
                   vtmb-console.log.
Profile Loads, Watch Loads, Probe Fades - developer tools; slow the game down.


TROUBLESHOOTING
---------------
- To check that it is active: set "Logging = 1", play one level change, quit,
  and open vtmb-fastfs.log in the game folder. Each fix reports
  "installed ... 1" (active) or "2" (switched itself off: different game files).
- If you add, remove or update mods or game files, nothing needs to be done:
  the caches notice the change and rebuild (the next load is a bit slower).
- If something seems wrong, switch fixes off one by one in the ini to find the
  one responsible, or set "Enable = 0".


SOURCE AND LICENCES
-------------------
The full source is in the "source" folder of this archive (build.sh builds it
with clang/lld). It includes libdeflate 1.22 by Eric Biggers, used under the
MIT licence (see source\LICENSE-libdeflate.txt).
