#include "desktop_pause.h"
#include "rcr_widescreen.h"
#include "config.h"
#include "controller.h"
#include "keybinds.h"
#include <stdio.h>
#include "nes_video.h"
#include <stdlib.h>
#include <string.h>

typedef enum { MAIN, OPTIONS, CONTROLS, CHARACTERS, CONFIRM_QUIT, CAPTURE, CONFLICT, DEFAULTS } Page;
typedef enum { NONE, UP, DOWN, LEFT, RIGHT, ACCEPT, BACK } Action;
typedef struct {
    Page page;
    int row, match, player, pad, input, conflict_player, conflict_action, confirm;
    const char *notice;
} Menu;
static const int action_order[]={4,5,6,7,0,1,3,2};
static const char *const action_names[]={"Punch","Kick","Select","Start","Up","Down","Left","Right"};
static int binding_action(const Menu *m) { return action_order[m->row-1]; }

static const char *const main_rows[] = {"Resume", "Options", "Controls", "Characters", "Quit game"};
static const char *const option_rows[] = {"Widescreen", "CRT filter", "Reduce sprite flicker"};
static const char *const quit_rows[] = {"Cancel", "Quit game"};
static const struct { NesAspectMode mode; const char *label; } aspects[] = {
    {NES_ASPECT_STOCK,"Off"}, {NES_ASPECT_FIT,"Fit to window"}, {NES_ASPECT_16_9,"16:9"},
    {NES_ASPECT_21_9,"21:9"}, {NES_ASPECT_32_9,"32:9"}
};

static int aspect_index(void) {
    for (int i = 0; i < NES_ASPECT_COUNT; ++i)
        if (aspects[i].mode == nes_video_aspect_mode()) return i;
    return 0;
}

static void change_option(int row, int direction) {
    if (row == 0) {
        int i = (aspect_index() + direction + NES_ASPECT_COUNT) % NES_ASPECT_COUNT;
        const char *name = nes_video_aspect_name(aspects[i].mode);
        rcr_ws_arg("--widescreen", name);
        SDL_strlcpy(g_nes_config.widescreen_mode, name, sizeof(g_nes_config.widescreen_mode));
        config_save_display_setting(DISPLAY_WIDESCREEN_MODE);
    } else if (row == 1) {
        g_nes_config.crt_filter ^= 1;
        config_save_display_setting(DISPLAY_CRT);
    } else {
        g_nes_config.reduce_sprite_flicker ^= 1;
        config_save_display_setting(DISPLAY_FLICKER);
    }
}

enum { PANEL_WIDTH = 252, PANEL_HEIGHT = 216 };
static const uint32_t paper = 0xffebeee8, ink = 0xff081018, accent = 0xfff8c460;

static void lettering(uint32_t *pixels, int x, int y, const char *text, uint32_t color) {
    for (; *text; ++text, x += 8) {
        for (int row = 0; row < 8; ++row) {
            unsigned bits = rcr_font_row(*text, row);
            for (int col = 0; col < 8; ++col)
                if (bits & (128u >> col)) pixels[(y+row)*PANEL_WIDTH+x+col] = color;
        }
    }
}

/* Long device and binding names can occupy the full detail line. */
static void field(uint32_t *p, int x, int y, const char *value, int chars, uint32_t color) {
    char buf[32];
    snprintf(buf,sizeof(buf),"%.*s",chars,value);
    lettering(p,x,y,buf,color);
}
static int selected_swatch(const Menu *m) {
    int choice=(m->row==2 ? g_nes_config.hair : g_nes_config.outfit)[m->player];
    return (unsigned)choice<RCR_SWATCH_COUNT ? choice : 0;
}

static void controls_panel(uint32_t *p, const Menu *m) {
    int player=m->player+1, connected;
    const char *device=controller_player_name(player,&connected);
    char label[160];
    snprintf(label,sizeof(label),"Player %d",player);
    lettering(p,19,25,label,paper);
    if (!m->row) lettering(p,6,25,">",accent);
    snprintf(label,sizeof(label),"%s%s",connected?"":"Offline ",device);
    field(p,10,39,label,29,paper);
    lettering(p,12,56,"Action",accent);
    lettering(p,73,56,"Keyboard",accent);
    lettering(p,158,56,"Controller",accent);
    for(int i=0;i<8;++i) {
        int action=action_order[i], y=70+i*12;
        lettering(p,12,y,action_names[action],paper);
        keybinds_label(player,action,0,label,sizeof(label));
        field(p,73,y,label,9,paper);
        keybinds_label(player,action,1,label,sizeof(label));
        field(p,158,y,label,11,paper);
        if (m->row==i+1) lettering(p,m->pad?148:63,y,">",accent);
    }
    if (m->row==9) lettering(p,5,169,">",accent);
    lettering(p,18,169,"Restore player defaults",paper);
    if (m->notice) field(p,10,182,m->notice,29,accent);
    else if (m->row>0 && m->row<9) {
        keybinds_label(player,binding_action(m),m->pad,label,sizeof(label));
        field(p,10,182,label,29,accent);
    }
    lettering(p,10,194,"Jump: Punch + Kick",paper);
    lettering(p,10,204,"Run: double tap Left or Right",paper);
}

static void characters_panel(uint32_t *p, const Menu *m) {
    int choice=selected_swatch(m);
    for(int i=0;i<2;++i) {
        int x=18+104*i;
        rcr_standing_preview(p,PANEL_WIDTH,x+39,68,i);
        lettering(p,x,83,i?"Ryan (P2)":"Alex (P1)",paper);
        if (m->player==i) lettering(p,x-10,83,">",accent);
    }
    if (m->row) lettering(p,8,m->row==2 ? 122 : 109,">",accent);
    lettering(p,20,109,"Shirt",paper);
    lettering(p,85,109,rcr_swatch_name(g_nes_config.outfit[m->player]),accent);
    lettering(p,20,122,"Hair",paper);
    lettering(p,85,122,rcr_swatch_name(g_nes_config.hair[m->player]),accent);
    for(int i=0;i<RCR_SWATCH_COUNT;++i) {
        int x=21+i*30;
        for(int y=0;y<16;++y) for(int dx=0;dx<22;++dx) {
            int border=dx==0||dx==21||y==0||y==15;
            int inner=dx==1||dx==20||y==1||y==14;
            p[(139+y)*PANEL_WIDTH+x+dx]=border?(choice==i?paper:ink):inner?ink:rcr_swatch_color(m->player,i,m->row==2);
        }
    }
}

static void draw_menu(SDL_Renderer *r, SDL_Texture *texture, Menu menu) {
    static uint32_t pixels[PANEL_WIDTH * PANEL_HEIGHT];
    int width=116, height=112;
    const char *title="Controls";
    switch(menu.page) {
    case MAIN: title=menu.match?"Match menu":"Paused"; break;
    case OPTIONS: width=236; height=80; title="Options"; break;
    case CONFIRM_QUIT: height=64; title="Quit game?"; break;
    case CONTROLS: width=PANEL_WIDTH; height=PANEL_HEIGHT; break;
    case CHARACTERS: width=212; height=167; title="Characters"; break;
    case CAPTURE: case DEFAULTS: case CONFLICT: width=248; height=94; break;
    }
    for (int y=0;y<height;++y) for(int x=0;x<width;++x)
        pixels[y*PANEL_WIDTH+x]=(x<2||x>=width-2||y<2||y>=height-2)?paper:ink;
    lettering(pixels,12,9,title,accent);
    if(menu.page==CONTROLS) controls_panel(pixels,&menu);
    else if(menu.page==CHARACTERS) characters_panel(pixels,&menu);
    else if(menu.page==CAPTURE || menu.page==CONFLICT || menu.page==DEFAULTS) {
        char line[64];
        if(menu.page==CAPTURE) {
            snprintf(line,sizeof(line),"P%d %s",menu.player+1,action_names[binding_action(&menu)]);
            lettering(pixels,14,29,line,paper);
            lettering(pixels,14,44,menu.pad?"Press a controller button":"Press a key",paper);
            lettering(pixels,14,67,menu.notice?menu.notice:"Escape to cancel",accent);
        } else {
            if(menu.page==CONFLICT) snprintf(line,sizeof(line),"Used by P%d %s",menu.conflict_player,action_names[menu.conflict_action]);
            else snprintf(line,sizeof(line),"Restore P%d defaults?",menu.player+1);
            lettering(pixels,14,30,line,paper);
            lettering(pixels,24,52,"Cancel",paper);
            lettering(pixels,24,70,menu.page==CONFLICT?"Swap":"Restore",paper);
            lettering(pixels,11,menu.confirm?70:52,">",accent);
        }
    } else {
        int rows=menu.page==MAIN?5:menu.page==OPTIONS?3:2;
        const char *const *labels=menu.page==MAIN?main_rows:menu.page==OPTIONS?option_rows:quit_rows;
        for(int i=0;i<rows;++i) {
            int y=28+16*i;
            if(i==menu.row) lettering(pixels,7,y,">",accent);
            const char *label=labels[i];
            if(menu.match && menu.page==OPTIONS && i!=1) label=i==0?"Match aspect":"Match flicker setting";
            lettering(pixels,20,y,label,paper);
            if(menu.page==OPTIONS) {
                const char *value=aspects[aspect_index()].label;
                if(i==1) value=g_nes_config.crt_filter?"On":"Off";
                if(i==2) value=g_nes_config.reduce_sprite_flicker?"On":"Off";
                lettering(pixels,width-10-(int)strlen(value)*8,y,value,paper);
            }
        }
    }
    /* Scale the completed pixel art once, including fractional CRT layouts. */
    int w,h;
    SDL_RenderGetLogicalSize(r, &w, &h);
    int scale = h/240;
    SDL_Rect src = {0, 0, width, height};
    SDL_Rect dst = {(w-width*scale)/2, (h-height*scale)/2, width*scale, height*scale};
    SDL_UpdateTexture(texture, &src, pixels, PANEL_WIDTH*sizeof(*pixels));
    SDL_RenderCopy(r, texture, &src, &dst);
}

static Action event_action(const SDL_Event *e) {
    if (e->type == SDL_KEYDOWN && !e->key.repeat) {
        switch (e->key.keysym.sym) {
        case SDLK_UP: return UP;
        case SDLK_DOWN: return DOWN;
        case SDLK_LEFT: return LEFT;
        case SDLK_RIGHT: return RIGHT;
        case SDLK_RETURN: case SDLK_KP_ENTER: return ACCEPT;
        case SDLK_ESCAPE: return BACK;
        }
    }
    if (e->type == SDL_CONTROLLERBUTTONDOWN) {
        switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return UP;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return DOWN;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return LEFT;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return RIGHT;
        case SDL_CONTROLLER_BUTTON_A: return ACCEPT;
        case SDL_CONTROLLER_BUTTON_B: return BACK;
        }
    }
    return NONE;
}

/* Menu navigation stays usable after the player remaps gameplay actions. */
static Action raw_action(const SDL_Event *e) {
    if (e->type != SDL_JOYBUTTONDOWN && e->type != SDL_JOYHATMOTION) return NONE;
    if (SDL_GameControllerFromInstanceID(e->jdevice.which)) return NONE;
    if (e->type == SDL_JOYHATMOTION) {
        switch (e->jhat.value) {
        case SDL_HAT_UP: return UP;
        case SDL_HAT_DOWN: return DOWN;
        case SDL_HAT_LEFT: return LEFT;
        case SDL_HAT_RIGHT: return RIGHT;
        default: return NONE;
        }
    }
    int button = controller_raw_input(e->jbutton.which, e->jbutton.button);
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A: return ACCEPT;
    case SDL_CONTROLLER_BUTTON_B: return BACK;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return RIGHT;
    default: return NONE;
    }
}

static void finish_binding(Menu *m) {
    m->notice=keybinds_assign(m->player+1,binding_action(m),m->pad,m->input,
        m->conflict_player,m->conflict_action)?NULL:"Could not save bindings";
    m->page=CONTROLS;
}

static void capture_event(Menu *m,const SDL_Event *e) {
    if(e->type==SDL_KEYDOWN && e->key.keysym.sym==SDLK_ESCAPE) {
        m->page=CONTROLS; m->notice=NULL; return;
    }
    int input=-1;
    if(!m->pad && e->type==SDL_KEYDOWN && !e->key.repeat) {
        input=e->key.keysym.scancode;
        if(keybinds_keyboard_reserved(input)) { m->notice="Reserved key"; return; }
    }
    if(m->pad) {
        SDL_JoystickID id;
        if(e->type==SDL_CONTROLLERBUTTONDOWN) { id=e->cbutton.which; input=e->cbutton.button; }
        else if(e->type==SDL_JOYBUTTONDOWN && !SDL_GameControllerFromInstanceID(e->jbutton.which)) {
            id=e->jbutton.which; input=controller_raw_input(id,e->jbutton.button);
        } else if(e->type==SDL_JOYHATMOTION && !SDL_GameControllerFromInstanceID(e->jhat.which)) {
            id=e->jhat.which;
            switch(e->jhat.value) {
            case SDL_HAT_UP: input=SDL_CONTROLLER_BUTTON_DPAD_UP; break;
            case SDL_HAT_DOWN: input=SDL_CONTROLLER_BUTTON_DPAD_DOWN; break;
            case SDL_HAT_LEFT: input=SDL_CONTROLLER_BUTTON_DPAD_LEFT; break;
            case SDL_HAT_RIGHT: input=SDL_CONTROLLER_BUTTON_DPAD_RIGHT; break;
            }
        } else return;
        if(!controller_instance_is_player(id,m->player+1)) return;
    }
    if(input<0) return;
    m->input=input; m->conflict_player=0;
    for(int player=1;player<=2;++player) for(int action=0;action<8;++action) {
        if(m->pad && player!=m->player+1) continue;
        if(player==m->player+1 && action==binding_action(m)) continue;
        if(keybinds_uses(player,action,m->pad,input)) {
            m->conflict_player=player; m->conflict_action=action;
            m->page=CONFLICT; m->confirm=0; return;
        }
    }
    finish_binding(m);
}

static int navigate(Menu *m, Action action) {
    if(action==NONE) return 1;
    m->notice=NULL;
    if(m->page==CONFLICT || m->page==DEFAULTS) {
        if(action==UP||action==DOWN||action==LEFT||action==RIGHT) m->confirm^=1;
        if(action==BACK || action==ACCEPT) {
            if(action==ACCEPT && m->confirm) {
                if(m->page==CONFLICT) finish_binding(m);
                else if(!keybinds_restore(m->player+1)) m->notice="Could not save bindings";
            }
            m->page=CONTROLS;
        }
        return 1;
    }
    int rows=m->page==MAIN?5:m->page==CONTROLS?10:(m->page==OPTIONS || m->page==CHARACTERS)?3:2;
    if(action==UP || action==DOWN) m->row=(m->row+(action==DOWN?1:-1)+rows)%rows;
    if(action==BACK) {
        if(m->page==MAIN) return 0;
        m->row=m->page==CONTROLS?2:m->page==CHARACTERS?3:m->page==CONFIRM_QUIT?4:1;
        m->page=MAIN; return 1;
    }
    int direction=action==LEFT?-1:action==RIGHT?1:0;
    switch(m->page) {
    case MAIN:
        if(action==ACCEPT) {
            static const Page pages[]={MAIN,OPTIONS,CONTROLS,CHARACTERS,CONFIRM_QUIT};
            if(!m->row) return 0;
            m->page=pages[m->row]; m->row=0; m->player=0; m->pad=0;
        }
        break;
    case OPTIONS:
        if(direction && (!m->match||m->row==1)) change_option(m->row,direction);
        break;
    case CONFIRM_QUIT:
        if(action==ACCEPT) {
            if(m->row) return -1;
            m->page=MAIN; m->row=4;
        }
        break;
    case CONTROLS:
        if(!m->row && (direction||action==ACCEPT)) m->player^=1;
        else if(m->row>0 && m->row<9) {
            if(direction) m->pad=direction>0;
            if(action==ACCEPT) m->page=CAPTURE;
        } else if(m->row==9 && action==ACCEPT) { m->page=DEFAULTS; m->confirm=0; }
        break;
    case CHARACTERS:
        if(direction || action==ACCEPT) {
            if(!m->row) m->player^=1;
            else {
                int change=direction?direction:1;
                int *choices=m->row==2 ? g_nes_config.hair : g_nes_config.outfit;
                choices[m->player]=(selected_swatch(m)+change+RCR_SWATCH_COUNT)%RCR_SWATCH_COUNT;
                config_save_display_setting(m->row==2 ? (m->player?DISPLAY_RYAN_HAIR:DISPLAY_ALEX_HAIR) :
                                                       (m->player?DISPLAY_RYAN_SHIRT:DISPLAY_ALEX_SHIRT));
            }
        }
        break;
    default: break;
    }
    return 1;
}

void game_pause_menu(SDL_Window *window, SDL_Renderer *renderer, const uint32_t *frame,
                     int width, void (*draw_scene)(const uint32_t *, int)) {
    size_t bytes = (size_t)width*240*sizeof(*frame);
    uint32_t *frozen = malloc(bytes);
    if (!frozen) return;
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, PANEL_WIDTH, PANEL_HEIGHT);
    if (!texture) { free(frozen); return; }
    memcpy(frozen, frame, bytes);
    Menu menu = {.page = MAIN};
    int dirty = 1, focused = 1;
    for (;;) {
        if (dirty) {
            draw_scene(frozen, width);
            draw_menu(renderer, texture, menu);
            SDL_RenderPresent(renderer);
            dirty = 0;
        }
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 100)) continue;
        controller_handle_event(&e);
        if (e.type == SDL_QUIT) exit(0);
        if (e.type == SDL_WINDOWEVENT && e.window.windowID == SDL_GetWindowID(window)) {
            if (e.window.event == SDL_WINDOWEVENT_CLOSE) exit(0);
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) focused = 0;
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) focused = 1;
            dirty = 1;
        }
        if(e.type==SDL_CONTROLLERDEVICEADDED || e.type==SDL_CONTROLLERDEVICEREMOVED ||
           e.type==SDL_JOYDEVICEADDED || e.type==SDL_JOYDEVICEREMOVED) dirty=1;
        if(menu.page==CAPTURE) {
            if(focused) capture_event(&menu,&e);
            dirty=1;
            continue;
        }
        Action raw = raw_action(&e);
        Action action = focused ? (raw != NONE ? raw : event_action(&e)) : NONE;
        if (action != NONE) {
            int result = navigate(&menu, action);
            if (result < 0) exit(0);
            if (!result) break;
            dirty = 1;
        }
    }
    SDL_DestroyTexture(texture);
    free(frozen);
}

#ifdef NESRECOMP_NET
/* The network input pump owns the match clock. Handle one event at a time
 * there and composite over live frames until the player closes the menu. */
static SDL_Texture *match_texture;
static Menu match_menu;

int game_match_menu_active(void) { return match_texture != NULL; }

void game_match_menu_close(void) {
    if (match_texture) SDL_DestroyTexture(match_texture);
    match_texture = NULL;
}

int game_match_menu_event(SDL_Renderer *renderer, const SDL_Event *e, int focused) {
    if (!match_texture) {
        if (focused && e->type == SDL_KEYDOWN && !e->key.repeat && e->key.keysym.sym == SDLK_ESCAPE) {
            match_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                SDL_TEXTUREACCESS_STREAMING, PANEL_WIDTH, PANEL_HEIGHT);
            match_menu = (Menu){.page = MAIN, .match = 1};
        }
        return 1;
    }
    if(match_menu.page==CAPTURE) { if(focused) capture_event(&match_menu,e); return 1; }
    Action raw = raw_action(e);
    if (!focused) return 1;
    int result = navigate(&match_menu, raw == NONE ? event_action(e) : raw);
    if (result <= 0) game_match_menu_close();
    return result;
}

void game_match_menu_draw(SDL_Renderer *renderer) {
    if (match_texture) draw_menu(renderer, match_texture, match_menu);
}
#endif
