// Save states for the recompiled game.
//
// The game's call stack lives partly in native C frames, which cannot be saved. States
// are therefore only taken at one "safe point" (entry of libetc's VSync, which the game
// calls every frame from ordinary code; calls made from interrupt callbacks do not count).
// A save requested during a level load waits until the load ends. Loading restores memory and hardware, unwinds the
// native stack back to main() and resumes at the safe point in the interpreter; the
// interpreter walks the MIPS stack back up through the callers and every function they
// call runs recompiled again.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

constexpr int kStateSlots = 5;
constexpr int kAutoSlot = 0;  // slot 0 is the auto-save (the game's own save record); 1..5 are manual
constexpr int kChapterBase = 1000;  // slot kChapterBase + i: chapter i of state_chapter_names()

void state_request_save(int slot);
void state_request_load(int slot);
bool state_slot_exists(int slot);
std::string state_slot_label(int slot);  // "empty" or the save time
std::vector<std::string> state_chapter_names();  // "1: Cat1 (prsn)", ... in the order reached
// The auto-save's level for menus ("Sewers"), or "" when it is not known.
std::string state_autosave_level();
// The auto-save: the game's save record (what a memory card save holds, 136 bytes at 800A53D4),
// checked. False when there is none or it is damaged.
constexpr uint32_t kSaveRecordSize = 136;
bool state_read_autosave(uint8_t* record);
extern uint64_t g_level_render_frame;  // the last frame the level render ran (smooth.cpp)

// Auto-save triggers (see savestate.cpp).
void state_note_level_start();
void state_note_file_loaded(const std::string& name);

// Performs a pending load (and never returns if it succeeds). Called once per frame.
struct R3000;
void state_poll_load(R3000* c);

// main(): returns true (with the resume address) after a state was loaded.
bool state_take_resume(uint32_t* pc);
