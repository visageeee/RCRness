#include "desktop_pause.h"
#include "rcr_widescreen.h"
#include "config.h"
#include "controller.h"
#include "keybinds.h"
#include <stdio.h>
#include "nes_video.h"
#include <stdlib.h>
#include <string.h>

typedef enum { MAIN, OPTIONS, CONTROLS, CHARACTERS, CITY_MAP, STATE_SLOTS, SLOT_ACTION, OVERWRITE, CONFIRM_QUIT, CAPTURE, CONFLICT, DEFAULTS } Page;
typedef enum { NONE, UP, DOWN, LEFT, RIGHT, ACCEPT, BACK, ZOOM } Action;
typedef struct {
    Page page;
    int row, match, player, pad, input, conflict_player, conflict_action, confirm;
    int map_room, map_zoom_toggle;
    int slot, slot_status[RCR_STATE_SLOTS];
    const char *notice;
} Menu;
static const int action_order[]={4,5,6,7,0,1,3,2};
static const char *const action_names[]={"Punch","Kick","Select","Start","Up","Down","Left","Right"};
static int binding_action(const Menu *m) { return action_order[m->row-1]; }

static const Page main_pages[]={MAIN, OPTIONS, CONTROLS, CHARACTERS, CITY_MAP, STATE_SLOTS, CONFIRM_QUIT};
enum { MAIN_COUNT = sizeof(main_pages)/sizeof(main_pages[0]) };
static const char *const main_rows[MAIN_COUNT] = {"Resume", "Options", "Controls", "Characters", "Map", "Save states", "Quit game"};
/* Netplay skips the slot browser and keeps Quit as the last visible row. */
static int main_index(const Menu *m,int row) {
    return m->match && row==MAIN_COUNT-2 ? row+1 : row;
}

static void refresh_slots(Menu *m) {
    for (int i=0;i<RCR_STATE_SLOTS;i++) m->slot_status[i]=runner_slot_status(i+1);
}

static const char *const option_rows[] = {"Widescreen", "CRT filter", "Reduce sprite flicker", "Blood effects", "Persistent bodies", "Coin bounce sound"};
enum { OPTION_COUNT = sizeof option_rows / sizeof option_rows[0] };
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
    } else if (row == 2) {
        g_nes_config.reduce_sprite_flicker ^= 1;
        config_save_display_setting(DISPLAY_FLICKER);
    } else if (row == 3) {
        g_nes_config.blood_effects ^= 1;
        rcr_clear_blood_spray();
        config_save_display_setting(DISPLAY_BLOOD);
    } else if (row == 4) {
        g_nes_config.persistent_bodies ^= 1;
        config_save_display_setting(DISPLAY_BODIES);
    } else {
        g_nes_config.coin_bounce_sound ^= 1;
        config_save_display_setting(DISPLAY_COIN_BOUNCE_SOUND);
    }
}

enum { PANEL_WIDTH = 252, PANEL_HEIGHT = 216 };
static const uint32_t paper = 0xffebeee8, ink = 0xff081018, accent = 0xfff8c460;

static void draw_letters(uint32_t *pixels, int stride, int x, int y, const char *text, uint32_t color) {
    for (; *text; ++text, x += 8) {
        for (int row = 0; row < 8; ++row) {
            unsigned bits = rcr_font_row(*text, row);
            for (int col = 0; col < 8; ++col)
                if (bits & (128u >> col)) pixels[(y+row)*stride+x+col] = color;
        }
    }
}

static void lettering(uint32_t *pixels,int x,int y,const char *text,uint32_t color) {
    draw_letters(pixels,PANEL_WIDTH,x,y,text,color);
}

/* Long device and binding names can occupy the full detail line. */
static void field(uint32_t *p, int x, int y, const char *value, int chars, uint32_t color) {
    char buf[32];
    snprintf(buf,sizeof(buf),"%.*s",chars,value);
    lettering(p,x,y,buf,color);
}
static int selected_swatch(const Menu *m) {
    int choice=(m->row==5 ? g_nes_config.pants : m->row==2 ? g_nes_config.hair : g_nes_config.outfit)[m->player];
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
        if (g_nes_config.oni_mode) lettering(p,x,83,i?"Konoko P2":"Konoko P1",paper);
        else lettering(p,x,83,i?"Ryan (P2)":"Alex (P1)",paper);
        if (!g_nes_config.oni_mode && m->player==i) lettering(p,x-10,83,">",accent);
    }
    int mode_y=g_nes_config.oni_mode ? 109 : 174;
    lettering(p,20,mode_y,"Oni mode",paper);
    lettering(p,118,mode_y,g_nes_config.oni_mode?"On":"Off",accent);
    if (g_nes_config.oni_mode) {
        lettering(p,8,mode_y,">",accent);
        return;
    }
    if (m->row) lettering(p,8,109+13*(m->row-1),">",accent);
    lettering(p,20,109,"Shirt",paper);
    lettering(p,118,109,rcr_swatch_name(g_nes_config.outfit[m->player]),accent);
    lettering(p,20,122,"Hair color",paper);
    lettering(p,118,122,rcr_swatch_name(g_nes_config.hair[m->player]),accent);
    lettering(p,20,135,"Hair style",paper);
    lettering(p,118,135,rcr_hair_style_name(rcr_hair_style(m->player)),accent);
    lettering(p,20,148,"Eyes",paper);
    lettering(p,118,148,rcr_eye_style_name(rcr_eye_style(m->player)),accent);
    lettering(p,20,161,"Pants",paper);
    lettering(p,118,161,rcr_swatch_name(g_nes_config.pants[m->player]),accent);
    if (m->row==3 || m->row==4 || m->row==6) return;
    for(int i=0;i<RCR_SWATCH_COUNT;++i) {
        int x=21+i*30;
        for(int y=0;y<16;++y) for(int dx=0;dx<22;++dx) {
            int border=dx==0||dx==21||y==0||y==15;
            int inner=dx==1||dx==20||y==1||y==14;
            p[(189+y)*PANEL_WIDTH+x+dx]=border?(choice==i?paper:ink):inner?ink:rcr_swatch_color(m->row==5 ? !m->player : m->player,i,m->row==2);
        }
    }
}

/* Schematic positions only. Connections and names come from the ROM. */
static const struct { int col, y; } city_nodes[RCR_MAP_ROOMS] = {
    {0,0},{1,0},{2,0},{3,0},{3,1},{4,0},{5,0},{6,0},{6,1},{7,0},
    {7,2},{6,2},{5,2},{4,2},{3,2},{2,2},{1,2},{0,2},
    {0,3},{1,3},{2,3},{3,3},{4,3},{5,3},{6,3},{7,3},
    {7,4},{6,4},{5,4},{4,4},{3,4},{2,4},{1,4},{0,4},{1,5}
};

enum { CELL_W=128, CELL_H=72, ATLAS_W=CELL_W*8, ATLAS_H=CELL_H*6 };
static SDL_Rect area_card(int room) {
    return (SDL_Rect){city_nodes[room].col*CELL_W+8,city_nodes[room].y*CELL_H+4,CELL_W-16,CELL_H-10};
}

static void atlas_fill(uint32_t *pixels,SDL_Rect rect,uint32_t color) {
    for (int y=rect.y;y<rect.y+rect.h;++y)
        for (int x=rect.x;x<rect.x+rect.w;++x) pixels[y*ATLAS_W+x]=color;
}

static SDL_Texture *create_city_atlas(SDL_Renderer *renderer) {
    uint32_t *pixels=malloc(ATLAS_W*ATLAS_H*sizeof *pixels);
    if (!pixels) return NULL;
    atlas_fill(pixels,(SDL_Rect){0,0,ATLAS_W,ATLAS_H},ink);
    for (int i=0;i<RCR_MAP_ROOMS;++i) {
        SDL_Rect a=area_card(i);
        uint64_t exits=rcr_map_exits(i);
        for (int j=i+1;j<RCR_MAP_ROOMS;++j) if (exits & (UINT64_C(1)<<j)) {
            SDL_Rect b=area_card(j);
            SDL_Rect line={SDL_min(a.x,b.x)+a.w/2,SDL_min(a.y,b.y)+22,
                           abs(a.x-b.x)+2,abs(a.y-b.y)+2};
            atlas_fill(pixels,line,0xff91a5ac);
        }
    }
    for (int i=0;i<RCR_MAP_ROOMS;++i) {
        SDL_Rect card=area_card(i);
        char name[32];
        rcr_map_name(i,name);
        if (strstr(name,"Mall")) {
            SDL_Rect frame={card.x-2,card.y-2,card.w+4,card.h+4};
            atlas_fill(pixels,frame,0xff409cff);
            /* Inset the card so the blue remains visible under both outlines. */
            frame=(SDL_Rect){card.x+2,card.y+2,card.w-4,card.h-4};
            atlas_fill(pixels,frame,0xff172632);
        } else atlas_fill(pixels,card,0xff172632);
        rcr_map_panorama(i,pixels+(card.y+2)*ATLAS_W+card.x+2,ATLAS_W,card.w-4,40);
        if (!*name) snprintf(name,sizeof name,"Area %02d",i+1);
        /* Two centered caption lines fit the longest original place name. */
        char *second=NULL;
        if (strlen(name)>14) {
            second=name+14;
            while (second>name && *second!=' ') --second;
            if (second>name) *second++=0;
            else second=NULL;
        }
        int y=card.y+(second?43:48);
        draw_letters(pixels,ATLAS_W,card.x+(card.w-(int)strlen(name)*8)/2,y,name,paper);
        if (second) draw_letters(pixels,ATLAS_W,card.x+(card.w-(int)strlen(second)*8)/2,y+9,second,paper);
    }
    SDL_Texture *texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,
                                            SDL_TEXTUREACCESS_STATIC,ATLAS_W,ATLAS_H);
    if (texture && SDL_UpdateTexture(texture,NULL,pixels,ATLAS_W*sizeof *pixels)<0) {
        SDL_DestroyTexture(texture);
        texture=NULL;
    }
    free(pixels);
    return texture;
}

static void map_outline(SDL_Renderer *r,int room,SDL_Rect src,SDL_Rect dst,int margin) {
    if (room<0) return;
    SDL_Rect card=area_card(room);
    SDL_Rect box={dst.x+(card.x-src.x-margin)*dst.w/src.w,
                  dst.y+(card.y-src.y-margin)*dst.h/src.h,
                  (card.w+2*margin)*dst.w/src.w,(card.h+2*margin)*dst.h/src.h};
    SDL_RenderDrawRect(r,&box);
}

static int objective_lit(void) { return SDL_GetTicks()%1000 < 650; }

static void draw_objective(SDL_Renderer *r,int room,SDL_Rect src,SDL_Rect dst,int unit) {
    SDL_Rect card=area_card(room);
    int x=dst.x+(card.x+card.w/2-src.x)*dst.w/src.w;
    int y=dst.y+(card.y+22-src.y)*dst.h/src.h;
    int radius=4*unit;
    for (int dy=-radius;dy<=radius;++dy) {
        int half=radius-abs(dy);
        SDL_SetRenderDrawColor(r,8,16,24,255);
        SDL_RenderDrawLine(r,x-half,y+dy,x+half,y+dy);
        if (half>=unit) {
            SDL_SetRenderDrawColor(r,255,104,112,255);
            SDL_RenderDrawLine(r,x-half+unit,y+dy,x+half-unit,y+dy);
        }
    }
}

static void draw_city_map(SDL_Renderer *r,SDL_Texture *text,SDL_Texture **atlas,const Menu *m) {
    if (!*atlas) *atlas=create_city_atlas(r);
    int w,h;
    SDL_RenderGetLogicalSize(r,&w,&h);
    int unit=SDL_max(1,h/240);
    SDL_Rect panel={4*unit,4*unit,w-8*unit,h-8*unit};
    SDL_Rect window={panel.x+4*unit,panel.y+19*unit,panel.w-8*unit,panel.h-47*unit};
    Uint8 red,green,blue,alpha;
    SDL_GetRenderDrawColor(r,&red,&green,&blue,&alpha);
    SDL_SetRenderDrawColor(r,8,16,24,255);
    SDL_RenderFillRect(r,&panel);
    SDL_SetRenderDrawColor(r,235,238,232,255);
    SDL_RenderDrawRect(r,&panel);
    SDL_Rect src={0,0,ATLAS_W,ATLAS_H};
    float sx,sy;
    SDL_RenderGetScale(r,&sx,&sy);
    int detail=(window.w*sx<820 || window.h*sy<346) != m->map_zoom_toggle;
    if (detail) {
        SDL_Rect card=area_card(m->map_room);
        src.w=SDL_min(ATLAS_W,window.w/unit);
        src.h=SDL_min(ATLAS_H,window.h/unit);
        src.x=SDL_clamp(card.x+card.w/2-src.w/2,0,ATLAS_W-src.w);
        src.y=SDL_clamp(card.y+card.h/2-src.h/2,0,ATLAS_H-src.h);
    }
    SDL_Rect dst=window;
    if (dst.w*src.h>dst.h*src.w) dst.w=dst.h*src.w/src.h;
    else dst.h=dst.w*src.h/src.w;
    dst.x+=(window.w-dst.w)/2;
    dst.y+=(window.h-dst.h)/2;
    int here=rcr_map_current_room();
    int objective=rcr_map_objective_room();
    if (*atlas) {
        SDL_RenderCopy(r,*atlas,&src,&dst);
        SDL_Rect old_clip;
        SDL_bool clipped=SDL_RenderIsClipEnabled(r);
        SDL_RenderGetClipRect(r,&old_clip);
        SDL_RenderSetClipRect(r,&dst);
        map_outline(r,m->map_room,src,dst,3);
        SDL_SetRenderDrawColor(r,248,196,96,255);
        map_outline(r,here,src,dst,1);
        if (objective>=0 && objective_lit()) draw_objective(r,objective,src,dst,unit);
        SDL_RenderSetClipRect(r,clipped?&old_clip:NULL);
    }
    SDL_SetRenderDrawColor(r,red,green,blue,alpha);
    char name[32],heading[64],location[32];
    rcr_map_name(m->map_room,name);
    snprintf(heading,sizeof heading,"%02d  %s",m->map_room+1,*name?name:"City map");
    if (here>=0) snprintf(location,sizeof location,"Here %02d",here+1);
    else SDL_strlcpy(location,"No active location",sizeof location);
    uint32_t captions[PANEL_WIDTH*24];
    for (size_t i=0;i<sizeof captions/sizeof *captions;++i) captions[i]=ink;
    field(captions,0,0,heading,30,paper);
    lettering(captions,0,8,location,accent);
    if (objective>=0) {
        char goal[16];
        snprintf(goal,sizeof goal,"Next %02d",objective+1);
        lettering(captions,80,8,goal,0xffff6870);
    }
    lettering(captions,184,8,"Mall",0xff409cff);
    lettering(captions,0,16,"Tab or X: zoom   Enter: home",paper);
    SDL_Rect lines={0,0,PANEL_WIDTH,24};
    SDL_UpdateTexture(text,&lines,captions,PANEL_WIDTH*sizeof *captions);
    lines.h=8;
    SDL_Rect band={panel.x+6*unit,panel.y+6*unit,SDL_min(PANEL_WIDTH,panel.w/unit-12)*unit,8*unit};
    lines.w=band.w/unit;
    SDL_RenderCopy(r,text,&lines,&band);
    lines.y=8; lines.h=16; band.y=panel.y+panel.h-21*unit; band.h=16*unit;
    SDL_RenderCopy(r,text,&lines,&band);
}

static void move_on_map(Menu *m,Action action) {
    if (action==ZOOM) { m->map_zoom_toggle^=1; return; }
    int here=rcr_map_current_room();
    if (action==ACCEPT && here>=0) { m->map_room=here; return; }
    int room=m->map_room;
    uint64_t exits=rcr_map_exits(room);
    for (int dest=0;dest<RCR_MAP_ROOMS;++dest) {
        if (!(exits & (UINT64_C(1)<<dest))) continue;
        int dx=city_nodes[dest].col-city_nodes[room].col;
        int dy=city_nodes[dest].y-city_nodes[room].y;
        if ((dx<0 && action==LEFT) || (dx>0 && action==RIGHT) ||
            (dy<0 && action==UP) || (dy>0 && action==DOWN)) {
            m->map_room=dest;
            return;
        }
    }
}

static void draw_menu(SDL_Renderer *r, SDL_Texture *texture, SDL_Texture **atlas, Menu menu) {
    static uint32_t pixels[PANEL_WIDTH * PANEL_HEIGHT];
    int width=116, height=112;
    const char *title="Controls";
    switch(menu.page) {
    case MAIN: height=menu.match?128:144; title=menu.match?"Match menu":"Paused"; break;
    case STATE_SLOTS: width=188; height=212; title="Save states"; break;
    case SLOT_ACTION: case OVERWRITE: width=192; height=92; title="Save states"; break;
    case OPTIONS: width=236; height=32+16*OPTION_COUNT; title="Options"; break;
    case CONFIRM_QUIT: height=64; title="Quit game?"; break;
    case CITY_MAP: draw_city_map(r,texture,atlas,&menu); return;
    case CONTROLS: width=PANEL_WIDTH; height=PANEL_HEIGHT; break;
    case CHARACTERS: width=244; height=g_nes_config.oni_mode ? 132 : 212; title="Characters"; break;
    case CAPTURE: case DEFAULTS: case CONFLICT: width=248; height=94; break;
    }
    for (int y=0;y<height;++y) for(int x=0;x<width;++x)
        pixels[y*PANEL_WIDTH+x]=(x<2||x>=width-2||y<2||y>=height-2)?paper:ink;
    lettering(pixels,12,9,title,accent);
    if(menu.page==CONTROLS) controls_panel(pixels,&menu);
    else if(menu.page==CHARACTERS) characters_panel(pixels,&menu);
    else if(menu.page==STATE_SLOTS) {
        for (int i=0;i<RCR_STATE_SLOTS;i++) {
            char label[16];
            int y=26+i*14;
            snprintf(label,sizeof label,"Slot %02d",i+1);
            lettering(pixels,20,y,label,paper);
            int status=menu.slot_status[i];
            lettering(pixels,95,y,status==SLOT_SAVED?"Saved":status==SLOT_EMPTY?"Empty":"Unavailable",paper);
            if (i==menu.row) lettering(pixels,8,y,">",accent);
        }
        if (menu.notice) lettering(pixels,12,195,menu.notice,accent);
    } else if(menu.page==SLOT_ACTION || menu.page==OVERWRITE) {
        char label[32];
        int confirm=menu.page==OVERWRITE;
        snprintf(label,sizeof label,confirm?"Replace slot %02d?":"Slot %02d",menu.slot);
        lettering(pixels,12,25,label,paper);
        lettering(pixels,22,42,confirm?"Cancel":"Save",paper);
        if (confirm || menu.slot_status[menu.slot-1]==SLOT_SAVED)
            lettering(pixels,22,58,confirm?"Replace":"Load",paper);
        lettering(pixels,9,42+16*menu.row,">",accent);
        if (menu.notice) lettering(pixels,12,76,menu.notice,accent);
    } else if(menu.page==CAPTURE || menu.page==CONFLICT || menu.page==DEFAULTS) {
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
        int rows=menu.page==MAIN?MAIN_COUNT-menu.match:menu.page==OPTIONS?OPTION_COUNT:2;
        const char *const *labels=menu.page==MAIN?main_rows:menu.page==OPTIONS?option_rows:quit_rows;
        for(int i=0;i<rows;++i) {
            int y=28+16*i;
            if(i==menu.row) lettering(pixels,7,y,">",accent);
            const char *label=labels[menu.page==MAIN?main_index(&menu,i):i];
            if(menu.match && menu.page==OPTIONS && (i==0 || i==2)) label=i==0?"Match aspect":"Match flicker setting";
            lettering(pixels,20,y,label,paper);
            if(menu.page==OPTIONS) {
                const char *value=aspects[aspect_index()].label;
                if(i==1) value=g_nes_config.crt_filter?"On":"Off";
                if(i==2) value=g_nes_config.reduce_sprite_flicker?"On":"Off";
                if(i==3) value=g_nes_config.blood_effects?"On":"Off";
                if(i==4) value=g_nes_config.persistent_bodies?"On":"Off";
                if(i==5) value=g_nes_config.coin_bounce_sound?"On":"Off";
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
        case SDLK_TAB: return ZOOM;
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
        case SDL_CONTROLLER_BUTTON_X: return ZOOM;
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
    case SDL_CONTROLLER_BUTTON_X: return ZOOM;
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
    if (m->page==SLOT_ACTION || m->page==OVERWRITE) {
        int confirm=m->page==OVERWRITE;
        if (action==UP || action==DOWN)
            m->row=(confirm || m->slot_status[m->slot-1]==SLOT_SAVED) ? !m->row : 0;
        if (action==BACK || (confirm && action==ACCEPT && !m->row)) {
            m->page=confirm ? SLOT_ACTION : STATE_SLOTS;
            m->row=confirm ? 0 : m->slot-1;
        } else if (action==ACCEPT) {
            if (!confirm && m->row==1) {
                if (runner_use_slot(m->slot,0)) return 0;
                m->notice="Could not load slot";
            } else {
                int status=runner_slot_status(m->slot);
                if (status==SLOT_UNAVAILABLE) m->notice="Slot unavailable";
                else if (status==SLOT_SAVED && !confirm) {
                    m->page=OVERWRITE;
                    m->row=0;
                } else if (runner_use_slot(m->slot,1)) {
                    m->page=STATE_SLOTS;
                    m->row=m->slot-1;
                    refresh_slots(m);
                    m->notice="State saved";
                } else m->notice="Could not save slot";
            }
        }
        return 1;
    }
    if (m->page==CITY_MAP && action!=BACK) { move_on_map(m,action); return 1; }
    int rows=m->page==MAIN?MAIN_COUNT-m->match:m->page==STATE_SLOTS?RCR_STATE_SLOTS:m->page==CONTROLS?10:m->page==CHARACTERS?(g_nes_config.oni_mode?1:7):m->page==OPTIONS?OPTION_COUNT:2;
    if(action==UP || action==DOWN) m->row=(m->row+(action==DOWN?1:-1)+rows)%rows;
    if(action==BACK) {
        if(m->page==MAIN) return 0;
        for (int i=0;i<MAIN_COUNT-m->match;++i)
            if (main_pages[main_index(m,i)]==m->page) { m->row=i; break; }
        m->page=MAIN; return 1;
    }
    int direction=action==LEFT?-1:action==RIGHT?1:0;
    switch(m->page) {
    case MAIN:
        if(action==ACCEPT) {
            if(!m->row) return 0;
            m->page=main_pages[main_index(m,m->row)];
            if (m->page==STATE_SLOTS) refresh_slots(m);
            if (m->page==CITY_MAP) { int here=rcr_map_current_room(); m->map_room=here<0?0:here; }
            m->row=0; m->player=0; m->pad=0;
        }
        break;
    case STATE_SLOTS:
        if(action==ACCEPT) {
            m->slot=m->row+1;
            int status=runner_slot_status(m->slot);
            m->slot_status[m->row]=status;
            if (status==SLOT_UNAVAILABLE) m->notice="Slot unavailable";
            else {
                m->page=SLOT_ACTION;
                m->row=status==SLOT_SAVED;
            }
        }
        break;
    case OPTIONS:
        if(direction && (!m->match||m->row==1||m->row>=3)) change_option(m->row,direction);
        break;
    case CONFIRM_QUIT:
        if(action==ACCEPT) {
            if(m->row) return -1;
            m->page=MAIN; m->row=MAIN_COUNT-1-m->match;
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
            if(g_nes_config.oni_mode || m->row==6) {
                g_nes_config.oni_mode=!g_nes_config.oni_mode;
                config_save_display_setting(DISPLAY_ONI_MODE);
                m->row=g_nes_config.oni_mode ? 0 : 6;
            } else if(!m->row) m->player^=1;
            else if (m->row==3) {
                int style=rcr_hair_style(m->player);
                g_nes_config.hair_style[m->player]=(style+(direction<0?-1:1)+RCR_HAIR_STYLE_COUNT)%RCR_HAIR_STYLE_COUNT;
                config_save_display_setting(m->player?DISPLAY_RYAN_STYLE:DISPLAY_ALEX_STYLE);
            } else if (m->row==4) {
                int eye=rcr_eye_style(m->player);
                g_nes_config.eyes[m->player]=(eye+(direction<0?-1:1)+RCR_EYE_STYLE_COUNT)%RCR_EYE_STYLE_COUNT;
                config_save_display_setting(m->player?DISPLAY_RYAN_EYES:DISPLAY_ALEX_EYES);
            } else {
                int change=direction?direction:1;
                int *choices=m->row==5 ? g_nes_config.pants : m->row==2 ? g_nes_config.hair : g_nes_config.outfit;
                choices[m->player]=(selected_swatch(m)+change+RCR_SWATCH_COUNT)%RCR_SWATCH_COUNT;
                config_save_display_setting(m->row==5 ? (m->player?DISPLAY_RYAN_PANTS:DISPLAY_ALEX_PANTS) :
                                            m->row==2 ? (m->player?DISPLAY_RYAN_HAIR:DISPLAY_ALEX_HAIR) :
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
    SDL_Texture *atlas=NULL;
    Menu menu = {.page = MAIN};
    int dirty = 1, focused = 1, last_lit = -1;
    for (;;) {
        if (menu.page==CITY_MAP) {
            int lit=objective_lit();
            if (lit!=last_lit) dirty=1;
            last_lit=lit;
        }
        if (dirty) {
            draw_scene(frozen, width);
            draw_menu(renderer, texture, &atlas, menu);
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
    SDL_DestroyTexture(atlas);
    SDL_DestroyTexture(texture);
    free(frozen);
}

#ifdef NESRECOMP_NET
/* The network input pump owns the match clock. Handle one event at a time
 * there and composite over live frames until the player closes the menu. */
static SDL_Texture *match_texture, *match_atlas;
static Menu match_menu;

int game_match_menu_active(void) { return match_texture != NULL; }

void game_match_menu_close(void) {
    if (match_texture) SDL_DestroyTexture(match_texture);
    SDL_DestroyTexture(match_atlas);
    match_atlas=NULL;
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
    if (match_texture) draw_menu(renderer, match_texture, &match_atlas, match_menu);
}
#endif
