#pragma once
#include <SDL.h>
#include <stdint.h>

enum { RCR_STATE_SLOTS = 12 };
enum { SLOT_UNAVAILABLE=-1, SLOT_EMPTY, SLOT_SAVED };
int runner_slot_status(int slot);
int runner_use_slot(int slot, int save);

void game_pause_menu(SDL_Window *window, SDL_Renderer *renderer, const uint32_t *frame,
                     int width, void (*draw_scene)(const uint32_t *, int));
void game_pause_preview(uint32_t *out, int width, const uint32_t *frozen, int old_width);

#ifdef NESRECOMP_NET
int game_match_menu_active(void);
int game_match_menu_event(SDL_Renderer *renderer, const SDL_Event *e, int focused);
void game_match_menu_draw(SDL_Renderer *renderer);
void game_match_menu_close(void);
#endif
