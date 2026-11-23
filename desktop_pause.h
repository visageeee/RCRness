#pragma once
#include <SDL.h>
#include <stdint.h>

void game_pause_menu(SDL_Window *window, SDL_Renderer *renderer, const uint32_t *frame,
                     int width, void (*draw_scene)(const uint32_t *, int));
void game_pause_preview(uint32_t *out, int width, const uint32_t *frozen, int old_width);

#ifdef NESRECOMP_NET
int game_match_menu_active(void);
int game_match_menu_event(SDL_Renderer *renderer, const SDL_Event *e, int focused);
void game_match_menu_draw(SDL_Renderer *renderer);
void game_match_menu_close(void);
#endif
