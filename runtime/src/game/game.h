// Apocalypse-specific enhancements: the 60 Hz simulation, draw distance, the game's menus,
// subtitles, cheats, and replacement textures, pictures and movies.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "recomp.h"

// ---------------------------------------------------------------- 60 Hz simulation (sim60.cpp)

extern int g_sim60_setting;    // 0 = 30 Hz, 1 = 60 Hz in levels 1-4, 2 = 60 Hz in every level
extern bool g_sim60_force;     // --sim60
extern bool g_sim60_fps30;     // frame rate 30 chosen: no 60 Hz simulation
extern std::string g_sim60_level;
extern bool g_sim60_demo;
void sim60_apply();
void sim60_note_file(const std::string& file);

// ---------------------------------------------------------------- native 3D (mesh.cpp)

extern bool g_native_3d;  // setting: the level mesh drawn natively, with float geometry (needs precise 3D)

// ---------------------------------------------------------------- draw distance (drawdist.cpp)

extern int g_draw_distance;  // percent of the game's own view distance (100 = original)
void drawdist_apply(R3000* c);

// ---------------------------------------------------------------- the game's menus (game_menu.cpp)

extern int g_menu_zoom;  // setting: the game's menus at this percent of their size
void game_menu_reset(bool state_loaded);  // a saved state was loaded / a level starts
// Shows `text` (at most 10 characters) the way the game shows a pickup.
void game_message(R3000* c, const char* text);
// A subtitle for `seconds` on a random side, tilted; subtitles.cpp decides when.
void game_subtitle(R3000* c, const char* text, float seconds);
void game_subtitle_keep(R3000* c);  // once per frame: shows a cleared subtitle again
void game_voice_poll(R3000* c);     // once per frame: queued pickup lines

// ---------------------------------------------------------------- subtitles (subtitles.cpp)

extern bool g_subtitles;  // setting
void subtitle_note_xa(uint32_t lba, int chan);  // cdrom.cpp: an XA sector is being played
void subtitle_poll(R3000* c);                   // once per frame
float subtitle_tilt(uint32_t text);
bool voice_clip_end(uint32_t lba, int chan, uint32_t* end);
bool voice_clip_lba(int block, int chan, uint32_t* lba, uint32_t* end);

// ---------------------------------------------------------------- cheats (cheats.cpp)
// 0 health, 1 lives, 2 weapons, 3 ammo, 4 smart bombs, 5 one hit kill. Off at start, not saved.

bool cheat_on(int which);
void cheat_set(int which, bool on);
void cheats_frame(R3000* c);  // once per frame
bool cheat_quiet();           // the cheat is giving a weapon: no voice line
void weapons_check(R3000* c, uint32_t player);      // a freed weapon left in a slot is dropped
void cheat_note_pickup(uint32_t player, int type);  // a weapon pickup (8005E260)

// ---------------------------------------------------------------- texture dump (texdump.cpp)

extern bool g_tex_dump;  // settings screen -> Dump textures (not saved)
void texdump_note(int page_x, int page_y, int depth, int clut_x, int clut_y, const uint16_t* vram);
void texdump_vram_changed(int x, int y, int w, int h);

// ---------------------------------------------------------------- texture replacements (texreplace.cpp)
// assets/replacements/textures; the game's font from assets/fonts

extern bool g_tex_replace;  // setting
extern bool g_game_font;    // setting
void texreplace_vram_changed(int x = 0, int y = 0, int w = 1024, int h = 512);  // (default: all of it)
void texreplace_atlas_lost();  // the renderer's atlas is gone (shut down)
void texreplace_vram_written(int x, int y, int w, int h);  // any upload or copy: palettes it read may change
// 0 = the game's texture, 1 = its replacement, 2 = with a letter drawn in the game font
int texreplace_find(int page_x, int page_y, int depth, int clut_x, int clut_y, float u0, float v0, float u1,
                    float v1, const int tw[4], const uint16_t* vram);
void texreplace_font_changed();      // after changing g_game_font
std::string texreplace_font_name();  // the font's name ("" = none in assets/fonts)
bool texreplace_wad_file(const std::string& name, std::vector<uint8_t>& out);  // a file of CD.WAD

// ---------------------------------------------------------------- replacement pictures (replace.cpp)
// Uploads and copies in VRAM, the whole VRAM re-sent, and the look for known pictures at each VBlank.

void replace_note_upload(int x, int y, int w, int h);
void replace_note_copy(int sx, int sy, int dx, int dy, int w, int h);
void replace_note_vram_reset();
void replace_note_poly(int area_y);
void replace_check(const uint16_t* vram);
bool replace_title_shown(int display_y);
bool replace_original_picture_shown(int display_y);
bool replace_load_image(const std::string& path, std::vector<uint32_t>& rgba, int& w, int& h);

// ---------------------------------------------------------------- replacement movies (movie.cpp)
// assets/replacements/fmvs

void movie_cd_sector(uint32_t lba);
bool movie_replacing();
bool movie_audio(int16_t* l, int16_t* r);
void movie_vblank(bool movie_picture_shown);
const uint32_t* movie_picture(int* w, int* h);
