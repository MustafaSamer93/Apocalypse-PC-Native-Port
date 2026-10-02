assets\image                  REQUIRED - your own dump of Apocalypse (USA): its .cue and .bin
                                files (any names), or only the .bin files
assets\bios                   optional - a US PlayStation BIOS (e.g. SCPH1001.BIN, 512 KB):
                                plays the PlayStation boot sequence at start
assets\replacements\textures  optional - HD textures: <file>\<index>.png for every texture of the
                                game files, screens\<name>.png for the full-screen pictures
                                (title, loading screens, logos), as tools\psx_textures.py
                                exports them. Used whenever present; anything missing comes
                                from the disc (F1 -> Texture replacements to compare)
assets\replacements\fmvs      optional - HD movies: <NAME>.mkv or .mp4 plays instead of the
                                disc's <NAME>.STR (INTRO, ATVILOGO, SEWER, ...)

Run build.bat from the project folder; it makes repocalypse.exe next to this folder.
No game data or BIOS is distributed with this project.
