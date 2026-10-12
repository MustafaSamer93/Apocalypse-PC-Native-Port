assets\image                  REQUIRED - your own dump of Apocalypse (USA): its .cue and .bin
                                files (any names), or only the .bin files
assets\replacements\textures  optional - HD textures: <file>\<index>.png for every texture of the
                                game files, screens\<name>.png for the full-screen pictures
                                (title, loading screens, logos), as tools\psx_textures.py
                                exports them. Used whenever present; anything missing comes
                                from the disc (Settings -> Texture replacements to compare)
assets\replacements\fmvs      optional - HD movies: <NAME>.mkv or .mp4 plays instead of the
                                disc's <NAME>.STR (INTRO, ATVILOGO, SEWER, ...)
assets\fonts                  for building only - a .ttf or .otf font for the game's own
                                letters (Recomp Enhancements -> Font type); the build puts
                                it in the exe, players never need a font file
assets\icon.png               optional - the exe's icon (a PNG up to 256 x 256), used by the
                                build instead of the game's lives icon

Run build.bat (Windows) or build.sh (Linux) from the project folder; it makes repocalypse.exe
(or repocalypse) next to this folder.
No game data is distributed with this project.
