#ifndef QUEST_MENU_BRIDGE_H
#define QUEST_MENU_BRIDGE_H

// What the game's C code needs from the quest menu: the pause menu's quest page shows a QUESTS scroll in the unused
// slot between the quiver and the bomb bag, and A on it opens the quest menu.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool QuestMenu_PauseEntryShown(void);
void QuestMenu_OpenFromPause(void);
// True when quests opened up since the quest menu was last opened in this save (the red mark on the entry).
bool QuestMenu_HasNews(void);
// Save slot bookkeeping (z_sram_NES.c): the "seen" state belongs to the save, not to the slot.
void QuestMenu_ForgetSave(int fileNum);
void QuestMenu_CopySave(int from, int to);

#ifdef __cplusplus
}
#endif

#endif // QUEST_MENU_BRIDGE_H
