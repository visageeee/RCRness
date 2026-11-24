/* Experimental River City Ransom USA E9C387EC widescreen mod:
 * world-space presentation and visibility.
 * Guest graphics come from the verified owner ROM at runtime. The optional
 * Konoko costume has its own hand-drawn tiles. See docs/WIDESCREEN.md. */
#include "rcr_widescreen.h"
#include "nes_runtime.h"
#include "nes_video.h"
#include "mapper.h"
#include "config.h"
#ifdef NESRECOMP_NET
#include "nes_netplay.h"
#endif
#include "mod_function_hooks.h"
#include "mod_savestate.h"
#include "debug_server.h"
#include <SDL.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#define ROOMS RCR_MAP_ROOMS
#define OBJECTS 7
#define TILES_PER_OBJECT 128
#define MAX_PAGES 5
#define PRG_SIZE 0x20000
#define CHR_SIZE 0x20000
typedef struct { int16_t x, y; uint8_t tile, attr, carried_owner, regions, effect; uint16_t tile_source, attr_source; } Sprite;
typedef struct {
    int16_t x, wy;
    uint32_t order;
    uint8_t type, anim, variant, flags, held_by, held_state, held_kind, room, valid, item_art, headless;
} Source;
/* The last two flags use padding in the legacy 20-byte saved source. */
_Static_assert(sizeof(Source)==20, "actor save source layout");
typedef struct {
    Sprite tiles[TILES_PER_OBJECT];
    /* Source addresses and signed ROM-space offsets of the two grip tiles.
     * The native OAM builder reads the same offsets before flipping/clipping. */
    struct { uint16_t offsets, tile; int8_t x,y; } saw_grip[2];
    Source source;
    uint16_t count;
    uint32_t order;
    uint8_t room;
} Packet;
typedef struct {
    Packet shadow[OBJECTS], present[OBJECTS];
    uint32_t sequence;
    uint8_t room;
    int16_t shadow_camera, present_camera;
} ActorState;
static ActorState s_actors;
static uint32_t s_list_pending;
static uint8_t s_rom[PRG_SIZE + CHR_SIZE];
static int s_ready, s_enabled = 1;
static NesAspectMode s_aspect = NES_ASPECT_FIT;
static int s_room = -1, s_view, s_camera;
typedef struct {
    int pages, resident;
    uint8_t tiles[MAX_PAGES * 32][30], attributes[MAX_PAGES * 32][30];
    uint8_t page_palette[MAX_PAGES], page_chr[MAX_PAGES];
    uint8_t resident_nt[2048], resident_chr[2];
} Terrain;
static Terrain live_terrain;
static uint8_t s_opaque[NES_MAX_RENDER_WIDTH * 240];
static uint64_t s_wide_frames, s_decode_errors, s_collision_reads, s_activation_reads, s_spawn_reads;
static unsigned s_terrain_pixels,s_terrain_diffs;

static int camera(void) { return (g_ram[0xdc] | (g_ram[0xdd] << 8)) - g_ram[0x3b]; }
/* $E2B1 latches the playfield scroll byte for the subsequent PPU split.
 * Unwrap it around the world camera, even across a nametable boundary. */
static int native_camera(void) {
    int cam=(int16_t)camera();
    int delta=((g_ram[0xf6]-cam+128)&255)-128;
    return cam+delta;
}
static int world_x(int obj) { return (int16_t)(g_ram[0x83+obj] | (g_ram[0x8c+obj] << 8)); }
static int street_scene(void) {
    return s_ready && g_ram[0x42] < ROOMS && g_ram[0x41] == 0 &&
           g_ram[0x40] && !(g_ram[0x3e] & 0x40) &&
           !(g_ram[0x36] & 0x40);
}
static int gameplay(void) {
    return street_scene() && (g_ram[0x48] & 0x80);
}
static int pause_panel(void) {
    /* The native full-submenu palettes use $0C in buffer $0110; the world
     * palettes restore $0F on return. Unlike
     * gameplay flags, this covers level/status/help as well as inventory
     * and passwords, and derives correctly from guest RAM on state load. */
    return (g_ram[0x3f]&0x20) && g_ram[0x110]==0x0c;
}
/* Cosmetic coordinates are in eighths of a world pixel. A drop lands at the
 * victim's street depth, independently of their height above the street. */
enum { BLOOD_DROPS=1024, HIT_SPRAY=24, WEAPON_SPRAY=72, FINISHER_SPRAY=320,
       STREET_WIDTH=MAX_PAGES*256, STREET_TOP=32, STREET_BOTTOM=192 };
typedef struct { int x,y,vx,vy,floor; } BloodDrop;
static BloodDrop blood_drops[BLOOD_DROPS];
static uint8_t blood_stains[ROOMS][STREET_BOTTOM-STREET_TOP][STREET_WIDTH/8];
static uint8_t room_stained[ROOMS];
static int blood_drop_next, blood_room=-1;
static char blood_dir[1024], blood_path[1200];
/* Zero identifies the shared journal referenced by pre-isolation saves. */
static uint32_t blood_game=UINT32_MAX;
static FILE *blood_file;
static int blood_write_failed, blood_pending;

static int stain_pixel(int room,int x,int y) {
    if ((unsigned)x>=STREET_WIDTH || y<STREET_TOP || y>=STREET_BOTTOM) return 0;
    uint8_t *cell=&blood_stains[room][y-STREET_TOP][x/8];
    unsigned mask=1u<<(x%8);
    if (*cell&mask) return 0;
    *cell|=mask;
    room_stained[room]=1;
    return 1;
}

/* Each new world pixel has one four-byte journal entry. Only its room byte
 * has the high bit set, so a truncated final write cannot misalign later
 * entries. Appending also preserves stains from other running instances. */
static void load_stains(FILE *f) {
    int room=-1, pixel=0, digits=0, byte;
    while ((byte=fgetc(f))!=EOF) {
        if (byte&128) { room=byte&127; pixel=digits=0; }
        else if (room>=0 && room<ROOMS) {
            pixel=(pixel<<7)|byte;
            if (++digits==3) {
                stain_pixel(room,pixel%STREET_WIDTH,STREET_TOP+pixel/STREET_WIDTH);
                room=-1;
            }
        }
    }
}

static void select_blood_game(uint32_t game) {
    if (blood_file) fclose(blood_file);
    blood_file=NULL;
    blood_write_failed=blood_pending=0;
    blood_path[0]=0;
    blood_game=game;
    memset(blood_stains,0,sizeof blood_stains);
    memset(room_stained,0,sizeof room_stained);
    rcr_clear_blood_spray();
    if (game==UINT32_MAX || !blood_dir[0]) return;
    if (game==0) snprintf(blood_path,sizeof blood_path,"%srcr-blood-v1.bin",blood_dir);
    else snprintf(blood_path,sizeof blood_path,"%srcr-blood-%08x.bin",blood_dir,(unsigned)game);
    FILE *f=fopen(blood_path,"rb");
    if (f) { load_stains(f); fclose(f); }
}

void rcr_new_game(void) { select_blood_game(UINT32_MAX); }

static void reserve_blood_game(void) {
    if (blood_game!=UINT32_MAX || !blood_dir[0] || blood_write_failed) return;
    uint32_t id=(uint32_t)time(NULL);
    for (;;) {
        if (!id || id==UINT32_MAX) id=1;
        snprintf(blood_path,sizeof blood_path,"%srcr-blood-%08x.bin",blood_dir,(unsigned)id);
        FILE *reserved=fopen(blood_path,"wbx");
        if (reserved) { fclose(reserved); blood_game=id; return; }
        if (errno!=EEXIST) {
            perror("Creating street blood journal");
            blood_write_failed=1;
            return;
        }
        ++id;
    }
}

static void save_stain(int room,int x,int y) {
    if (!stain_pixel(room,x,y)) return;
    reserve_blood_game();
    if (!blood_path[0] || blood_write_failed) return;
    if (!blood_file) blood_file=fopen(blood_path,"ab");
    int pixel=(y-STREET_TOP)*STREET_WIDTH+x;
    uint8_t entry[]={128|room,pixel>>14,(pixel>>7)&127,pixel&127};
    if (!blood_file || fwrite(entry,1,sizeof entry,blood_file)!=sizeof entry) {
        perror("Saving street blood");
        blood_write_failed=1;
    } else blood_pending=1;
}

void rcr_clear_blood_spray(void) {
    memset(blood_drops,0,sizeof blood_drops);
    blood_drop_next=0;
    blood_room=-1;
}

static void blood_enter_room(int room) {
    if (room==blood_room) return;
    memset(blood_drops,0,sizeof blood_drops);
    blood_room=room;
}

static int weapon_impact(void) {
    /* Bank 4 $8CB1 passes the striking slot ($05) to the damage calculator.
     * Slots 4–6 deal contact damage as thrown objects. For a fighter, the
     * $8B36 attack table selects weapon power ($04AB) for these swings. */
    unsigned attacker=g_ram[0x05];
    if (attacker>=OBJECTS) return 0;
    if (attacker>=4) return 1;
    unsigned move=g_ram[0x4f+attacker]&0x7f;
    if (move==0x20 || move==0x21 || move==0x2a) return 1;
    /* A throw-power attack can register contact through the wielder before
     * release. $03B7 distinguishes a held item from a carried fighter. */
    unsigned held=g_ram[0x3b7+attacker];
    return (move==0x22 || move==0x23 || move==0x29) &&
           (g_ram[0x56+attacker]&0x40) && held>=4 && held<OBJECTS;
}

/* Bit per guest slot. An ordinary weapon remains responsible for all item
 * physics, AI, pickup, damage and throwing. Only its presentation is special. */
static uint8_t head_items, headless_fighters, saw_items, opening_saw;
static int debug_saw_pending;
enum { ITEM_STOCK, ITEM_HEAD, ITEM_SAW };
enum { HEAD_LOOSE=1, HEAD_HELD, SAW_LOOSE, SAW_HELD };
static int has_saw(int slot) {
    return slot>=4 && slot<OBJECTS && (saw_items&(1u<<slot));
}
static int wielding_saw(int fighter) {
    return (g_ram[0x56+fighter]&0x40) && has_saw(g_ram[0x3b7+fighter]);
}
void rcr_object_replaced(int obj) {
    saw_items &= ~(1u<<obj);
    head_items &= ~(1u<<obj);
    headless_fighters &= ~(1u<<obj);
}

/* The guest object arrays are seven bytes apart. Store extra street items
 * in world coordinates and give the closest pickups real guest slots. */
typedef struct {
    int16_t x, lane, height;
    int8_t vx, vz;
    uint8_t type, variant, anim, kind;
} StreetWeapon;
enum { STREET_SLOTS=8 };
static StreetWeapon street_items[STREET_SLOTS];
static int street_room=-1;

static void enter_street(int room) {
    if (street_room!=room) memset(street_items,0,sizeof street_items);
    street_room=room;
}

static int player_distance(int x,int lane) {
    int distance=INT_MAX;
    for (int player=0;player<2;++player) {
        if (!(g_ram[0x48+player]&0x80)) continue;
        int dx=abs(x-world_x(player));
        int dy=abs(lane-(int)(g_ram[0x9e + player]|(g_ram[0xa7+player]<<8)));
        if (dx+dy<distance) distance=dx+dy;
    }
    return distance;
}

static int ground_slot(int exclude) {
    int best=-1, best_distance=-1;
    for (int slot=4;slot<OBJECTS;++slot) {
        if (!(g_ram[0x48+slot]&0x80)) return slot;
        int type=g_ram[0xcb+slot];
        if (type<8 || type>15 || (g_ram[0x4f+slot]&0x7f) ||
            (g_ram[0x56+slot]&0xc0) || slot==exclude) continue;
        int distance=player_distance(world_x(slot),g_ram[0x9e + slot]|(g_ram[0xa7+slot]<<8));
        if (distance>best_distance) { best=slot; best_distance=distance; }
    }
    return best;
}

static StreetWeapon resting_weapon(int slot) {
    if (!(g_ram[0x48+slot]&0x80)) return (StreetWeapon){0};
    return (StreetWeapon){.x=world_x(slot),.lane=g_ram[0x9e + slot]|(g_ram[0xa7+slot]<<8),
        .type=g_ram[0xcb+slot],.variant=g_ram[0x48+slot]&127,.anim=g_ram[0xd3+slot],
        .kind=has_saw(slot) ? ITEM_SAW : (head_items>>slot)&1};
}

static int drop_head(int victim,int direction) {
    if (victim<2 || victim>3 || g_ram[0xcb+victim]>=8 || (headless_fighters&(1u<<victim))) return 0;
#ifdef NESRECOMP_NET
    if (nes_netplay_active()) return 0;
#endif
    /* The guest update counter and impact position give a reproducible 1-in-4 roll. */
    if ((g_ram[0x6a]+world_x(victim)+g_ram[0x9e + victim])&3) return 0;
    enter_street(g_ram[0x42]);
    int slot=ground_slot(g_ram[5]);
    if (slot<0 || (g_ram[0x48+slot]&0x80)) {
        int spare=0;
        while (spare<STREET_SLOTS && street_items[spare].type) ++spare;
        if (spare==STREET_SLOTS) return 0;
        if (slot<0) {
            street_items[spare]=(StreetWeapon){.x=world_x(victim),
                .lane=g_ram[0x9e + victim]|(g_ram[0xa7+victim]<<8),
                .height=20+g_ram[0xb9+victim]+(g_ram[0xc2+victim]<<8),
                .vx=direction*2,.vz=4,.type=14,.variant=6,.kind=ITEM_HEAD};
            headless_fighters |= 1u<<victim;
            return 1;
        }
        street_items[spare]=resting_weapon(slot);
    }
    /* Standard $0E throwable, released from head height. The guest's $04
     * flight state bounces, lands and becomes pickable. Speeds are 8.8. */
    const unsigned clear[]={0x5d,0x64,0x7a,0x95,0xb0,0x31d,0x32d,0x335,0x33d,
                            0x355,0x35d,0x365,0x36d,0x3b0};
    for (unsigned i=0;i<sizeof clear/sizeof clear[0];++i) g_ram[clear[i]+slot]=0;
    const unsigned position[]={0x83,0x8c,0x9e,0xa7};
    for (unsigned i=0;i<sizeof position/sizeof position[0];++i)
        g_ram[position[i]+slot]=g_ram[position[i]+victim];
    int z=(g_ram[0xb9+victim] | (g_ram[0xc2+victim]<<8))+20;
    g_ram[0xb9+slot]=(uint8_t)z; g_ram[0xc2+slot]=(uint8_t)(z>>8);
    g_ram[0xcb+slot]=14;
    g_ram[0x50b+slot]=g_ram[0x42];
    g_ram[0x4f+slot]=4;
    g_ram[0x56+slot]=0x80;
    g_ram[0x6f+slot]=direction<0 ? 0x80 : 0;
    g_ram[0xd3+slot]=0;
    g_ram[0x325+slot]=(uint8_t)(direction*2);
    g_ram[0x345+slot]=3;
    g_ram[0x34d+slot]=0x80;
    g_ram[0x30f+slot]=0x40;
    int attacker=g_ram[5];
    if (attacker>=4 && attacker<OBJECTS) attacker=g_ram[0x3b7+attacker];
    g_ram[0x3b7+slot]=(uint8_t)(attacker<4 ? attacker : victim);
    g_ram[0x6ed+slot]=1;
    g_ram[0x48+slot]=0x86;
    rcr_object_replaced(slot);
    head_items |= 1u<<slot;
    headless_fighters |= 1u<<victim;
    return 1;
}

enum { SAW_SPRAY=640 };
static void spray(int x,int floor,int height,int direction,unsigned drops,int weapon,int burst) {
    unsigned seed=(unsigned)(x+floor+blood_drop_next);
    for (unsigned i=0;i<drops;++i) {
        seed=seed*1664525u+1013904223u;
        int spread=(int)(seed>>28);
        BloodDrop *d=&blood_drops[blood_drop_next];
        blood_drop_next=(blood_drop_next+1)%BLOOD_DROPS;
        d->x=x*8;
        d->y=(floor-height-17+(spread&3))*8;
        d->vx=direction*(5+spread);
        d->vy=-5-(int)((seed>>21)&15);
        if (weapon) {
            d->vx+=direction*(int)((seed>>8)&15);
            d->vy-=(int)((seed>>12)&15);
        }
        if (burst) {
            d->vx=direction*(int)((seed>>9)&63)-direction*22;
            d->vy=-12-(int)((seed>>17)&55);
            d->x+=((int)(seed&7)-3)*8;
        }
        d->floor=floor+(int)((seed>>16)&15)-7;
    }
}

void rcr_blood_hit(int victim,int fatal) {
    if (!g_nes_config.blood_effects || !gameplay() || pause_panel()) return;
    blood_enter_room(g_ram[0x42]);
    int x=world_x(victim);
    int floor=240-(int16_t)(g_ram[0x9e + victim] | (g_ram[0xa7+victim]<<8));
    int height=(int16_t)(g_ram[0xb9+victim] | (g_ram[0xc2+victim]<<8));
    int direction=(g_ram[0xd3+victim]&0x80) ? -1 : 1;
    int weapon=weapon_impact();
    int severed=fatal && weapon && drop_head(victim,direction);
    int attacker=g_ram[5];
    int saw=weapon && (has_saw(attacker) || (attacker<4 && wielding_saw(attacker)));
    unsigned drops=saw ? SAW_SPRAY : severed ? FINISHER_SPRAY : weapon ? WEAPON_SPRAY : HIT_SPRAY;
    spray(x,floor,height,direction,drops,weapon,severed||saw);
}

static void place_ground_weapon(int slot,StreetWeapon item) {
    /* Restore a settled item. Guest state $80 handles normal pickup and
     * initializes carry/throw velocities when somebody takes the item. */
    const unsigned clear[]={0x56,0x5d,0x64,0x6f,0x7a,0x95,0xb0,0xb9,0xc2,0x30f,
        0x31d,0x325,0x32d,0x335,0x33d,0x345,0x34d,0x355,0x35d,0x365,0x36d,0x3b0,0x3b7};
    for (unsigned i=0;i<sizeof clear/sizeof clear[0];++i) g_ram[clear[i]+slot]=0;
    g_ram[0x83+slot]=(uint8_t)item.x; g_ram[0x8c+slot]=(uint16_t)item.x>>8;
    g_ram[0x9e + slot]=(uint8_t)item.lane; g_ram[0xa7+slot]=(uint16_t)item.lane>>8;
    g_ram[0xcb+slot]=item.type; g_ram[0xd3+slot]=item.anim;
    g_ram[0x50b+slot]=g_ram[0x42]; g_ram[0x6ed+slot]=1;
    g_ram[0x48+slot]=0x80|item.variant; g_ram[0x4f+slot]=0x80;
    rcr_object_replaced(slot);
    if (item.kind==ITEM_HEAD) head_items|=1u<<slot;
    if (item.kind==ITEM_SAW) saw_items|=1u<<slot;
}

/* Spawn an ordinary pickable $0E item, with its chainsaw identity kept
 * alongside the other custom artwork when the guest slots are busy. */
static int spawn_saw(int offset) {
    if (!gameplay() || pause_panel() || (g_ram[0x3e]&2)) return 0;
#ifdef NESRECOMP_NET
    if (nes_netplay_active()) return 0;
#endif
    enter_street(g_ram[0x42]);
    StreetWeapon saw={.type=14,.variant=6,.anim=6,.kind=ITEM_SAW,
        .x=world_x(0)+offset,.lane=g_ram[0x9e]|(g_ram[0xa7]<<8)};
    int slot=ground_slot(-1);
    if (slot>=0 && !(g_ram[0x48+slot]&0x80)) {
        place_ground_weapon(slot,saw);
        return 1;
    }
    for (int i=0;i<STREET_SLOTS;++i) {
        if (street_items[i].type) continue;
        if (slot>=0) {
            street_items[i]=resting_weapon(slot);
            place_ground_weapon(slot,saw);
        } else street_items[i]=saw;
        return 1;
    }
    return 0;
}

static void street_step(void) {
    if (!g_ram[0x40]) { enter_street(-1); return; }
    enter_street(g_ram[0x42]);
    if (!gameplay() || pause_panel() || (g_ram[0x3e]&2)) return;
#ifdef NESRECOMP_NET
    if (nes_netplay_active()) return;
#endif
    int nearest=-1, distance=INT_MAX;
    for (int i=0;i<STREET_SLOTS;++i) {
        StreetWeapon *item=&street_items[i];
        if (!item->type) continue;
        if (item->height>0) {
            item->x+=item->vx;
            item->height+=item->vz--;
            if (item->height>0) continue;
            item->height=0;
            item->anim=6;
        }
        int d=player_distance(item->x,item->lane);
        if (d<distance) { nearest=i; distance=d; }
    }
    if (nearest<0) return;
    int slot=ground_slot(-1);
    if (slot<0) return;
    StreetWeapon old=resting_weapon(slot);
    if (old.type && distance>=player_distance(old.x,old.lane)) return;
    StreetWeapon item=street_items[nearest];
    street_items[nearest]=old;
    place_ground_weapon(slot,item);
}

static void blood_step(void) {
    if (!g_nes_config.blood_effects) return;
    if (!g_ram[0x40]) {
        if (blood_room>=0) rcr_clear_blood_spray();
        return;
    }
    if (!gameplay() || pause_panel()) return;
    int room=g_ram[0x42];
    blood_enter_room(room);
    for (int i=0;i<BLOOD_DROPS;++i) {
        BloodDrop *d=&blood_drops[i];
        if (!d->floor) continue;
        d->x+=d->vx;
        d->y+=d->vy;
        d->vy+=3;
        if (d->y < d->floor*8) continue;
        int x=d->x/8;
        save_stain(room,x,d->floor);
        if (i&1) save_stain(room,x+1,d->floor);
        if (!(i%4)) save_stain(room,x,d->floor+1);
        d->floor=0;
    }
    if (blood_pending) {
        if (fflush(blood_file)!=0) {
            perror("Saving street blood");
            blood_write_failed=1;
        }
        blood_pending=0;
    }
}

static void draw_blood(uint32_t *out,int width,int view) {
    if (!g_nes_config.blood_effects || !gameplay() || pause_panel() ||
        (g_ram[0x3e]&2)) return;
    int room=g_ram[0x42];
    int left=view<0 ? 0 : view;
    int right=view+width>STREET_WIDTH ? STREET_WIDTH : view+width;
    if (room_stained[room] && left<right) {
        for (int y=STREET_TOP;y<STREET_BOTTOM;++y) {
            for (int byte=left/8;byte<=(right-1)/8;++byte) {
                unsigned bits=blood_stains[room][y-STREET_TOP][byte];
                for (int bit=0;bits;++bit,bits>>=1) {
                    int x=byte*8+bit;
                    if ((bits&1) && x>=left && x<right)
                        out[y*width+x-view]=((x+y)&3) ? 0xff852638 : 0xff652433;
                }
            }
        }
    }
    if (room!=blood_room) return;
    for (int i=0;i<BLOOD_DROPS;++i) {
        const BloodDrop *d=&blood_drops[i];
        if (!d->floor) continue;
        int x=d->x/8-view, y=d->y/8;
        if (x>=0 && x<width && y>=32 && y<192)
            out[y*width+x]=(i&1) ? 0xffdb3846 : 0xff952734;
    }
}

static void draw_street_items(uint32_t *out,int width,int view,int native);
static void draw_bodies(uint32_t *out,int width,int view,int native);

void rcr_draw_native_blood(uint32_t *out,int width) {
    draw_blood(out,width,native_camera());
    draw_bodies(out,width,native_camera(),1);
    draw_street_items(out,width,native_camera(),1);
}

static uint8_t prg(int bank, unsigned addr) {
    if (addr >= 0xc000 && addr <= 0xffff) return s_rom[0x1c000 + addr - 0xc000];
    if (bank >= 0 && bank < 8 && addr >= 0x8000 && addr < 0xc000)
        return s_rom[bank * 0x4000 + addr - 0x8000];
    return 0;
}
static unsigned word(int bank, unsigned addr) { return prg(bank, addr) | (prg(bank, addr+1) << 8); }

int rcr_map_current_room(void) {
    /* A shop or NES submenu retains its parent street in $42. */
    return s_ready && g_ram[0x40] && (g_ram[0x48]&0x80) && g_ram[0x42]<ROOMS ? g_ram[0x42] : -1;
}

int rcr_map_objective_room(void) {
    if (rcr_map_current_room()<0 || (g_ram[0x38]&0x20)) return -1;
    /* A route through the required encounters. Bank 4 $B502 maps rooms to
     * their waves, $B717 checks prerequisites, and bank 6 $A510 records
     * victories using the $A55F pairs (flag byte offset, mask). Both
     * Dragon Twins must fall before Slick. Slick triggers the ending via
     * $38 bit 5 at $9DB7 instead of setting a persistent defeat bit. */
    static const struct { uint8_t room, boss; } route[] = {
        {11,0x0c}, /* Rocko */
        {4,0x0f},  /* Blade */
        {15,0x0b}, /* Turk */
        {16,0x0a}, /* Mojo */
        {22,0x10}, /* Thor */
        {28,0x12}, /* Ivan */
        {29,0x11}, /* Otis */
        {32,0x14}, {32,0x15} /* Dragon Twins */
    };
    for (size_t i=0;i<sizeof route/sizeof route[0];++i) {
        unsigned entry=0xa55f+2*(route[i].boss-9);
        if (!(g_ram[0x44+prg(6,entry)] & prg(6,entry+1))) return route[i].room;
    }
    return 34; /* Slick, on the roof */
}

uint64_t rcr_map_exits(int room) {
    if (!s_ready || (unsigned)room>=ROOMS) return 0;
    /* Bank 1, $9E9D: room -> list of exit triggers. $9FAE distinguishes
     * shop actions from transitions and reads the destination entrance ID. */
    unsigned list=word(1,0xa748+2*room);
    uint64_t exits=0;
    for (int i=0; i<32; ++i) {
        unsigned trigger=word(1,list+i*2);
        if (trigger<0x8000 || trigger>=0xc000) break;
        if (prg(1,trigger)&0x80) continue;
        unsigned entry=prg(1,trigger+1);
        if (entry>=73) continue;
        unsigned dest=prg(1,0xa43d+5*entry);
        if (dest<ROOMS && dest!=(unsigned)room) exits |= UINT64_C(1)<<dest;
    }
    return exits;
}

void rcr_map_name(int room, char name[32]) {
    int n=0;
    if (s_ready && (unsigned)room<ROOMS) {
        unsigned id=prg(1,0x9cbb+room);
        if (id>=0x37 && id<=0x44) {
            unsigned ptr=word(3,0xb200+2*id);
            for (int i=0; i<40 && n<31; ++i) {
                unsigned c=prg(3,ptr+i);
                if (c==5) break;
                if (c>=0xc0 && c<0xda) name[n++]='A'+c-0xc0;
                else if (c>=0xda && c<0xf4) name[n++]='a'+c-0xda;
                else if (c>=0xf4 && c<=0xfd) name[n++]='0'+c-0xf4;
                else if (c==0xb4) name[n++]='-';
                else if (c==0xb6) name[n++]='.';
                else if (c==0 && n && name[n-1]!=' ') name[n++]=' ';
            }
        }
    }
    while (n && name[n-1]==' ') --n;
    name[n]=0;
}

static uint8_t screen_byte(unsigned bank, unsigned addr) {
    if (addr>=0x8000) return prg((int)bank,addr);
    unsigned offset=PRG_SIZE+bank*1024+addr-0x1000;
    return addr>=0x1000 && addr<0x2000 && offset<sizeof s_rom ? s_rom[offset] : 0;
}
/* C7F2..C928: bounded resident-screen bytecode, including substreams. */
static int screen_stream(uint8_t *nametables, unsigned bank, unsigned *src, unsigned *dst, int depth, int *budget) {
    if (depth>8) return 0;
    while (--*budget>0) {
        unsigned cmd=screen_byte(bank,(*src)++);
        if (!cmd) {
            unsigned hi=screen_byte(bank,(*src)++);
            if (!hi) return 1;
            unsigned lo=screen_byte(bank,(*src)++);
            if ((hi&0xf0)==0x20) *dst=(hi<<8)|lo;
            else {
                unsigned sub=(hi<<8)|lo;
                if (!screen_stream(nametables,bank,&sub,dst,depth+1,budget)) return 0;
            }
            continue;
        }
        unsigned count=cmd&63, rows=1;
        if ((cmd&0xc0)==0x80 || (cmd&0xc0)==0xc0) rows=screen_byte(bank,(*src)++);
        unsigned value=0;
        if (!(cmd&0x40)) value=screen_byte(bank,(*src)++);
        unsigned start=*src;
        for (unsigned r=0;r<rows;r++) {
            if ((cmd&0xc0)==0xc0) *src=start;
            for (unsigned i=0;i<count;i++) {
                if (--*budget<=0) return 0;
                if (cmd&0x40) value=screen_byte(bank,(*src)++);
                if (*dst>=0x2000 && *dst<0x2800) nametables[*dst-0x2000]=(uint8_t)value;
                (*dst)++;
            }
        }
    }
    return 0;
}

/* The scrolling-room directory is fixed-bank C599, three bytes per room.
 * C602..C704 contains seven-byte page descriptors. C230 streams twenty rows
 * of tiles, C41C streams the attributes; both reference owner-ROM bank 0..3.
 * C959's resident-screen records supply the other backgrounds. Directory
 * allocation separates paired scenes from centered single-screen scenes. */
static int decode_terrain(Terrain *t, int room_id) {
    memset(t,0,sizeof *t);
    unsigned directory = 0xc599 + room_id * 3;
    if (!(prg(7, directory+2) & 0x80)) {
        /* C000..C091 loads the resident C959 screen records into the two
         * horizontal nametables. Directory allocation distinguishes paired
         * scenes from single-screen scenes sharing the next record. */
        unsigned first=prg(7,directory+2), end=0x26;
        for (int room=0; room<ROOMS; room++) {
            unsigned next=prg(7,0xc59b+room*3);
            if (next>first && next<end) end=next;
        }
        int pages=end-first;
        if (pages==2 || pages==1) {
            for (int p=pages-1;p>=0;p--) {
                unsigned desc=0xc959+(first+p)*6;
                t->page_palette[p]=prg(7,desc+3);
                unsigned src=word(7,desc), dst=0x2000;
                int budget=16384;
                if (!screen_stream(t->resident_nt,prg(7,desc+2),&src,&dst,0,&budget)) {
                    return 0;
                }
            }
            unsigned desc=0xc959+first*6;
            t->resident_chr[0]=prg(7,desc+4)&0xfe;
            t->resident_chr[1]=prg(7,desc+5)&0xfe;
            t->resident=1; t->pages=pages;
        }
        return t->pages>0;
    }
    unsigned first = word(7, directory), end = 0xc705;
    for (int room=0; room<ROOMS; room++) {
        unsigned p = word(7, 0xc599+room*3);
        if (p > first && p < end) end=p;
    }
    if (first < 0xc602 || end <= first || (end-first)%7) return 0;
    t->pages = (int)((end-first)/7);
    if (t->pages > MAX_PAGES) return 0;
    /* The final corridor has four authored pages and a duplicate fifth page
     * for the native column stream's lookahead. The original right wall stops
     * Alex at X=1015. Cache capacity must not become another visible room. */
    if (room_id==0x20 && t->pages==5 &&
        !memcmp(s_rom+0x1c000+first-0xc000+21,
                s_rom+0x1c000+first-0xc000+28,7)) t->pages=4;
    for (int p=0; p<t->pages; p++) {
        unsigned desc = first+p*7;
        unsigned rows = word(7,desc), attrs = word(7,desc+2);
        int bank = prg(7,desc+6)&3;
        t->page_palette[p] = prg(7,desc+4);
        t->page_chr[p] = prg(7,desc+5);
        for (int y=4; y<24; y++) {
            unsigned row = word(bank,rows+y*2);
            if (row < 0x8000 || row > 0xbfe0 || attrs < 0x8000 || attrs > 0xbfc0) {
                return 0;
            }
            for (int x=0; x<32; x++) {
                t->tiles[p*32+x][y] = prg(bank,row+x);
                uint8_t a = prg(bank,attrs+(y/4)*8+x/4);
                t->attributes[p*32+x][y] = (a >> (((y&2)*2)+(x&2))) & 3;
            }
        }
    }
    return 1;
}
static void decode_room(void) {
    s_room=g_ram[0x42];
    if (!decode_terrain(&live_terrain,s_room)) {
        live_terrain.pages=0;
        ++s_decode_errors;
    }
    /* The game presents single-screen interiors in the native compositor. */
    if (live_terrain.resident && live_terrain.pages==1) live_terrain.pages=0;
}
static int view_left(int cam, int width) {
    int left = cam-(width-256)/2;
    int room_width=live_terrain.pages ? live_terrain.pages*256 : 256;
    if (room_width <= width) return -(width-room_width)/2;
    if (left < 0) left=0;
    if (left > room_width-width) left=room_width-width;
    return left;
}
static uint32_t terrain_pixel(const Terrain *t,int wx,int y,int *opaque,int live) {
    *opaque=0;
    if (wx < 0 || wx >= t->pages*256 || y<32 || y>=192) return 0xff000000u;
    int p=wx/256, pal;
    unsigned offset;
    if (t->resident) {
        unsigned nt=p*1024, tx=(wx&255)/8, ty=y/8;
        unsigned tile=t->resident_nt[nt+ty*32+tx];
        unsigned attr=t->resident_nt[nt+960+(ty/4)*8+tx/4];
        pal=(attr>>((ty&2)*2+(tx&2)))&3;
        offset=PRG_SIZE+t->resident_chr[tile/128]*1024+(tile%128)*16+(y&7);
    } else {
        int tile=t->tiles[wx/8][y/8];
        pal=t->attributes[wx/8][y/8];
        offset=PRG_SIZE+(t->page_chr[p]&0xfe)*1024+tile*16+(y&7);
    }
    if (offset+8>=sizeof s_rom) return 0xff000000u;
    int bit=7-(wx&7);
    int pixel=((s_rom[offset]>>bit)&1)|(((s_rom[offset+8]>>bit)&1)<<1);
    *opaque=pixel!=0;
    int color;
    if (live && (t->resident || t->page_palette[p]==g_ram[0x518])) color=g_ppu_pal[pixel ? pal*4+pixel : 0];
    else {
        unsigned palette=word(7,0xe6f1+t->page_palette[p]*2);
        color=prg(7,palette+(pixel ? pal*4+pixel : 0));
        if (live && (g_ram[0x3e]&2) && g_ram[0x692]<4 && color!=0x0f) {
            int brightness=g_ram[0x692]*16;
            if ((color&0x30)>=brightness) color=(color&15)|brightness;
        }
    }
    return g_nes_palette[color&63];
}

static uint32_t bg_pixel(int wx,int y,int *opaque) {
    return terrain_pixel(&live_terrain,wx,y,opaque,1);
}

/* Fit the whole 160-line playfield into the card, preserving proportions.
 * Average a 3x3 grid per output pixel so fine brickwork and signage survive. */
void rcr_map_panorama(int room,uint32_t *out,int stride,int width,int height) {
    if (!s_ready || (unsigned)room>=ROOMS) return;
    Terrain t;
    if (!decode_terrain(&t,room)) return;
    int world=t.pages*256;
    int w=SDL_min(width,world*height/160), h=SDL_min(height,160*width/world);
    out+=(height-h)/2*stride+(width-w)/2;
    for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
        unsigned r=0,g=0,b=0;
        for (int j=1;j<6;j+=2) for (int i=1;i<6;i+=2) {
            int opaque;
            uint32_t c=terrain_pixel(&t,(6*x+i)*world/(6*w),32+(6*y+j)*160/(6*h),&opaque,0);
            r+=(c>>16)&255; g+=(c>>8)&255; b+=c&255;
        }
        out[y*stride+x]=0xff000000u|(r/9<<16)|(g/9<<8)|(b/9);
    }
}

/* Hair and facial outlines share ink #1. These hand-selected regions in
 * unflipped tile coordinates restrict dye to the crown, fringe and sides.
 * Each byte covers one row, most significant bit at the left. */
static const uint8_t hair_region[256][8] = {
    [0x00]={255,255,255,255,255,0xe0}, [0x01]={255,255,255,255,0x1c,0x1c,0x1c,0x10},
    [0x60]={255,255,255,255,0xe0,0xe0}, [0x61]={255,255,255,255,0x1c,0x1c,0x10},
    [0x40]={255,255,255,255,0xe0}, [0x41]={255,255,255,255,255,255,255,0x1c},
    [0x62]={255,255,255,255}, [0x63]={255,255,255,255,255,255,255,0x1c},
    [0x09]={0,0,255,255,255,255,255,255}, [0x0a]={0,0,255,255,255,255,255,255},
    [0x19]={0x1f,0x1f}, [0x1a]={0x3e,0x1e,0x0e,0x0c,0x0c,0x04}, [0x9d]={0x3e,0x1e,0x0e,0x0e,0x0c,0x04},
    [0x0d]={0xf0,0x38,0x10}, [0x1e]={255,255,255,255,255,255,255,255},
    [0x1f]={255,255,0x0f,0x03,0x07,0x0f,0x7f,0x1f}, [0x2e]={0xf0,0xf0,0xf0,0x60,0x60},
    [0x2f]={0x1f,0x1f,0x03,0x01}, [0x90]={0,0,255,255,255,255,0xe0},
    [0x91]={0,0,255,255,255,255,255,255}, [0x64]={0,0,255,255,255,255},
    [0x65]={0,0,255,255,255,255,255,255}, [0xa1]={0x0f,0x0f,0x0f,0x0f},
    [0x9e]={255,255,0xfc,0xfc,0xf8,0xf8,0xe0,0x60}, [0x9f]={255,255,0x3f,0x1f,0x0f,0x07},
    [0x49]={0x0f,0x03,0x03}, [0x4a]={255,0x0f,0x3f,0x1f,0x0f,0x07,0x01},
    [0x67]={255,0x0f,0x38,0x0c,0x07,0x03,0x01}, [0x93]={0x0f,0x07,0x03,0x03,0x03,0x03,0x03},
    [0xd5]={255,255,0x0f,0x07,0x03,0x03,0x01},
    [0x45]={255,255,255,255,255,0xe0}, [0x46]={255,255,255,255,0x30,0x30,0x30},
    [0x77]={255,255,255,255,0xe0,0x40}, [0x78]={255,255,255,255,255,0x7c,0x7c,0x7c},
    [0xa2]={255,255,255,255,255,0xf3,255,0xf0}, [0xa7]={255,255,255,255,0x1f,0x1f,0x1f,0x1f},
};
/* Encode independent player owners in the low two bits (shirt) and next
 * two bits (face), followed by two bits each for pants and the full costume. The same tags travel with native and repaired OAM. */
enum { SHIRT = 1, HAIR = 4, PANTS = 16, BODY = 64 };
static uint8_t appearance_tag(const Sprite *s, int obj) {
    int owner=s->carried_owner ? s->carried_owner : obj+1;
    return s->effect ? s->effect : owner<=2 ? owner*s->regions : 0;
}
/* Each crown is drawn at a different angle in the ROM. Describe its
 * exposed edge in tile space, before the metasprite's horizontal/vertical
 * flip. Tiles containing just the lower fringe keep their original shape. */
enum { UPRIGHT=1, LOWERED, SIDE_RIGHT, SIDE_LEFT };
static const uint8_t crown_edge[256] = {
    [0x00]=UPRIGHT,[0x01]=UPRIGHT,[0x60]=UPRIGHT,[0x61]=UPRIGHT,
    [0x40]=UPRIGHT,[0x41]=UPRIGHT,[0x62]=UPRIGHT,[0x63]=UPRIGHT,
    [0x09]=LOWERED,[0x0a]=LOWERED,[0x90]=LOWERED,[0x91]=LOWERED,
    [0x64]=LOWERED,[0x65]=LOWERED,[0x1e]=UPRIGHT,[0x1f]=UPRIGHT,
    [0x9e]=UPRIGHT,[0x9f]=UPRIGHT,[0x45]=UPRIGHT,[0x46]=UPRIGHT,
    [0x77]=UPRIGHT,[0x78]=UPRIGHT,[0x49]=UPRIGHT,[0x4a]=SIDE_RIGHT,
    [0x67]=SIDE_RIGHT,[0x93]=UPRIGHT,[0xd5]=SIDE_RIGHT,
    [0xa2]=SIDE_LEFT,[0xa7]=SIDE_RIGHT,[0x0d]=UPRIGHT
};

int rcr_hair_style(int player) {
    int style=g_nes_config.hair_style[player];
    return (unsigned)style<RCR_HAIR_STYLE_COUNT ? style : 0;
}
/* Entries 0..9 retain their original config indices. The nine street
 * faces are assigned to gang members by slot ($B6A9), independently of
 * the gang's name. The Dragon Twins share one face bank. */
typedef struct {
    const char *name;
    int bank, variant;
    uint8_t head[4]; /* NPC upper-left/right and lower-left/right tiles */
    int eye_y, mirror;
} FaceSource;
static const FaceSource faces[]={
    {.name="Original"}, {.name="Crew cut"}, {.name="Spikes"},
    {"Moose",84,.variant=2}, {"Mojo",85,.variant=3}, {"Turk",86,.variant=2}, {"Rocko",87,.variant=3},
    {"Blade",90,.variant=2}, {"Thor",91,.variant=3}, {"Otis",100,.variant=2},
    {"Alex",0,.variant=0}, {"Ryan",0,.variant=1},
    {"Street 1",66,.variant=2}, {"Street 2",67,.variant=2}, {"Street 3",68,.variant=2},
    {"Street 4",70,.variant=2}, {"Street 5",71,.variant=2}, {"Street 6",74,.variant=3},
    {"Street 7",72,.variant=3}, {"Street 8",73,.variant=3}, {"Street 9",69,.variant=3},
    {"Benny",88,.variant=2}, {"Clyde",89,.variant=3}, {"Ivan",105,.variant=4}, {"Tex",106,.variant=4},
    {"Dragon Twins",110,.variant=4}, {"Slick",108,.variant=4},
    /* Street NPC metasprites $13..$19 use CHR page 83. */
    {"Roxy and Cyndi",83,-1,{28,29,44,45},.eye_y=7},
    {"Townsperson 1",83,-1,{34,35,50,51},.eye_y=7},
    {"Townsperson 2",83,-1,{36,37,52,53},.eye_y=7},
    {"Townsperson 3",83,-1,{38,39,54,55},.eye_y=7},
    {"Townsperson 4",83,-1,{20,21,22,23},.eye_y=7},
    /* Shop actor types $1D..$25. Bank 2 $B0CE picks the metasprite,
     * $B738/$B750 select its CHR pages. Align their eyes to fighter scale. */
    {"Clerk 1",115,-1,{0,1,16,17},8,1}, {"Clerk 2",121,-1,{0,1,16,17},8,1},
    {"Clerk 3",121,-1,{25,9,41,17},8,1}, {"Clerk 4",115,-1,{29,30,45,17},8,1},
    {"Chef",119,-1,{0,1,16,17},7,1}, {"Clerk 5",119,-1,{6,7,16,17},7,1},
    {"Clerk 6",119,-1,{8,9,16,17},7,1}, {"Clerk 7",119,-1,{11,12,43,44},.eye_y=6},
    {"Secret shop",121,-1,{14,15,30,31},.eye_y=8},
    {"Closed eyes",112,-1,{42,43,16,17},.eye_y=7}
};
enum { FACE_COUNT = sizeof faces / sizeof faces[0] };
/* The shop expression adds closed eyes to Alex's existing hairstyle. */
_Static_assert(FACE_COUNT==RCR_HAIR_STYLE_COUNT+1 && FACE_COUNT==RCR_EYE_STYLE_COUNT+2,
               "face catalog size");

int rcr_eye_style(int player) {
    int style=g_nes_config.eyes[player];
    return (unsigned)style<RCR_EYE_STYLE_COUNT ? style : 0;
}
const char *rcr_eye_style_name(int style) {
    return faces[style>0 && style<RCR_EYE_STYLE_COUNT ? style+2 : 0].name;
}
const char *rcr_hair_style_name(int style) {
    return faces[(unsigned)style<RCR_HAIR_STYLE_COUNT ? style : 0].name;
}

/* Copy only the donor rows needed by the compositor from the owner's ROM.
 * Each record holds eight pose tiles in the game's two-bitplane format. */
static uint8_t face_tiles[FACE_COUNT][8][16];
static void load_faces(void) {
    for (int style=3; style<FACE_COUNT; ++style) {
        const FaceSource *f=&faces[style];
        for (int pose=0;pose<8;++pose) {
            uint8_t *out=face_tiles[style][pose];
            if (f->variant>=0) {
                unsigned tile=prg(7,0xef99+pose*5+f->variant);
                static const unsigned player_pages[]={64,66,74,65};
                unsigned bank=f->bank ? (unsigned)f->bank : player_pages[tile/64];
                memcpy(out,s_rom+PRG_SIZE+bank*1024+(tile%64)*16,16);
                continue;
            }
            memset(out,0,16);
            for (int y=0;y<8;y++) for (int x=0;x<8;x++) {
                int sx=x+(pose%2)*8, sy=y;
                if (pose==4 || pose==5) sy-=2;
                if (pose==6) sy+=6;
                if (pose==7) { sx=15-y; sy=x; }
                if (sy<0) continue;
                sy=(sy*f->eye_y+3)/7;
                if (f->mirror) sx=15-sx;
                unsigned tile=f->head[(sy/8)*2+sx/8];
                const uint8_t *chr=s_rom+PRG_SIZE+f->bank*1024+tile*16+(sy%8);
                for (int plane=0;plane<2;plane++)
                    out[y+plane*8] |= ((chr[plane*8]>>(7-sx%8))&1)<<(7-x);
            }
        }
    }
}

/* Inverse of the two player columns of $EF99. These are the crown tiles
 * the metasprite decoder substitutes by actor variant. Other poses reuse
 * shared art, including the back of the head. Zero denotes a shared tile. */
static const uint8_t crown_pose[256]={
    [0x00]=1,[0x60]=1,[0x01]=2,[0x61]=2,
    [0x40]=3,[0x62]=3,[0x41]=4,[0x63]=4,
    [0x90]=5,[0x64]=5,[0x91]=6,[0x65]=6,
    [0xa0]=7,[0x66]=7,[0x4a]=8,[0x67]=8
};

/* Eye sockets in unflipped tile space. Row offsets follow the
 * lowered head and its continuation in the following tile. */
static const uint8_t sockets[8][8]={
    {0,0,0,0,0x03,0x1f,0x3f,0x3f},{0,0,0,0,0xe0,0xe0,0xe0,0xe0},
    {0,0,0,0,0x0f,0x3f,0x3f,0x3f},{0,0,0,0,0x80,0x80,0x80,0x80},
    {0,0,0,0,0,0,0x0f,0x3f},{0,0,0,0,0,0,0x80,0x80},
    {0x3f,0x3f}, {0,0x70,0,0xc0,0xe0,0xe0,0xe0,0xe0}
};

#include "oni_sprite.h"

/* $C0..$DF are outside the player outfit tag range. Four tiles make
 * the airborne head; the resting item has just two tiles in the guest OAM. */
enum { SAW_BLANK=175, SAW_ART=176, HEAD_ART=192, HEAD_COMPACT=208, CUT_TOP=224, CUT_BOTTOM=232, HIDDEN=240,
       MARK_HEAD=241, MARK_BODY=242, MARK_SAW=243, HELD_SAW_ART=244, HELD_SAW_END=252 };
static const char head_art[16][17]={
    "................",".....######.....","...##########...","..############..",
    "..#######ss###..","..##sss#ssss##..","..#s##ss##ss##..","..#sw#ssw#ss##..",
    "...sssssssss#...","...sss#sssss#...","....sssss#ss....",".....s##sss.....",
    "......ssss......","......rrr.......",".......r........","................"
};
static const char ground_head[8][17]={
    ".....#####......","...#########....","..#####ss####...","..##s#ss#ss##...",
    "..#sw#ssw#ss#...","...ssss#ssss....","....ss###ss.....",".....rrrss......"
};
static char head_ink(uint8_t tag,int row,int bit) {
    int part=tag&15;
    int x=(part&1)*8+((part&4)?bit:7-bit);
    int y=(part&8)?7-row:row;
    return tag>=HEAD_COMPACT ? ground_head[y][x] : head_art[(part&2)*4+y][x];
}

/* Two NES tiles: orange motor housing, open handle, toothed steel bar.
 * Vertical throws rotate the same artwork within the guest footprint. */
static int saw_art_tag(int tag) {
    return (tag>=SAW_BLANK && tag<HEAD_ART) || (tag>=HELD_SAW_ART && tag<HELD_SAW_END);
}

static char saw_ink(int tag,int row,int bit) {
    static const char art[8][17]={
        ".####...........",".#..#...........","######.#.#.#.#..",
        "#oooo##########.","#o#oo#sssssssss#","#oooo##########.",
        ".####..#.#.#.#..","................"};
    if (tag==SAW_BLANK) return '.';
    if (tag>=HELD_SAW_ART) {
        /* Work in blade coordinates so the chain travels out along the top
         * and back along the bottom, even when the carrier turns. $052B
         * advances with guest updates and is restored with the game state. */
        int part=tag-HELD_SAW_ART;
        int x=8*(part&1)+((part&2)?bit:7-bit);
        int phase=g_ram[0x52b]&3;
        int y=((part&4)?7-row:row)-(phase&1);
        if (y<0) return '.';
        if (x>=6 && x<15 && (y==2 || y==6)) {
            int tooth=(x+(y==2 ? 4-phase : phase))&3;
            return tooth==0 ? 's' : tooth==1 ? '#' : '.';
        }
        return art[y][x];
    }
    int part=tag-SAW_ART;
    int x=(part&4)?bit:7-bit, y=(part&8)?7-row:row;
    return (part&2) ? art[7-x][(part&1)*8+y] : art[y][(part&1)*8+x];
}

void rcr_style_sprite_row(uint8_t tag,uint8_t tile,int row,uint8_t *lo,uint8_t *hi) {
    if (saw_art_tag(tag)) {
        *lo=*hi=0;
        for (int bit=0;bit<8;++bit) {
            char c=saw_ink(tag,row,bit);
            if (c=='#' || c=='o') *lo|=1u<<bit;
            if (c=='s' || c=='o') *hi|=1u<<bit;
        }
        return;
    }
    if (tag>=HEAD_ART && !g_nes_config.blood_effects) return;
    if (tag>=CUT_TOP) {
        if (tag==HIDDEN || (tag<CUT_BOTTOM ? row<=(tag-CUT_TOP) : row>=7-(tag-CUT_BOTTOM))) *lo=*hi=0;
        return;
    }
    if (tag>=HEAD_ART) {
        *lo=*hi=0;
        for (int bit=0;bit<8;bit++) {
            char c=head_ink(tag,row,bit);
            if (c=='#' || c=='s' || c=='r') *lo |= 1u<<bit;
            if (c=='s' || c=='w') *hi |= 1u<<bit;
        }
        return;
    }
    if (g_nes_config.oni_mode && tag>>6) {
        if (oni_has_tile(tile)) {
            *lo=*hi=0;
            for (int bit=0;bit<8;++bit) {
                unsigned pen=oni_pen(oni_ink(tile,row,bit));
                *lo |= (pen&1)<<bit;
                *hi |= (pen>>1)<<bit;
            }
        }
        return;
    }
    unsigned owner=(tag>>2)&3;
    if (owner!=1 && owner!=2) return;
    unsigned pose=crown_pose[tile];
    int eyes=rcr_eye_style(owner-1);
    uint8_t eye_mask=eyes && pose ? sockets[pose-1][row] : 0;
    if (eye_mask) {
        const uint8_t *eye=face_tiles[eyes+2][pose-1]+row;
        uint8_t mask=eye_mask & (*lo|*hi) & (eye[0]|eye[8]);
        *lo=(*lo & ~mask) | (eye[0] & mask);
        *hi=(*hi & ~mask) | (eye[8] & mask);
    }
    int style=rcr_hair_style(owner-1);
    if (!style) return;
    if (style>=3) {
        uint8_t mask=hair_region[tile][row] & ~eye_mask;
        if (!pose || !mask) return;
        const uint8_t *donor=face_tiles[style][pose-1]+row;
        uint8_t donor_lo=donor[0], donor_hi=donor[8];
        /* White ink below the crown is an eye in either face. Leave those
         * pixels with the wearer. Ryan's glint lies in the top two rows. */
        if (row>=(pose==5 || pose==6 ? 4 : 2))
            mask &= ~((~*lo & *hi) | (~donor_lo & donor_hi));
        *lo=(*lo & ~mask) | (donor_lo & mask);
        *hi=(*hi & ~mask) | (donor_hi & mask);
        return;
    }
    static const uint8_t top[2][8]={
        {0xff,0xff,0x81}, {0xee,0xcc,0x88}
    };
    static const uint8_t side[2][8]={
        {7,7,7,7,3,3,1,1}, {7,3,1,0,7,3,1,0}
    };
    uint8_t cut;
    switch (crown_edge[tile]) {
    case UPRIGHT: cut=top[style-1][row]; break;
    case LOWERED: cut=row<2 ? 0 : top[style-1][row-2]; break;
    case SIDE_RIGHT: cut=side[style-1][row]; break;
    case SIDE_LEFT:
        cut=side[style-1][row];
        cut=((cut&1)<<7)|((cut&2)<<5)|((cut&4)<<3);
        break;
    default: return;
    }
    /* Ryan's crown glint includes one skin-colored highlight beside the
     * white one. Keep every other skin pixel, including in fallen poses. */
    int glint=((tile==0x60 || tile==0x62) && row==1) || (tile==0x64 && row==3);
    uint8_t removable=(*lo ^ *hi) | (glint ? 0x10 : 0);
    cut &= hair_region[tile][row] & removable & ~eye_mask;
    *lo &= ~cut;
    *hi &= ~cut;
}

static uint8_t shadow_tags[64], display_tags[64], coherent_tags[64];
static const struct { const char *name; uint8_t color; } swatches[RCR_SWATCH_COUNT] = {
    {"Original",0}, {"Red",0x16}, {"Gold",0x28}, {"Green",0x1a}, {"Blue",0x12}, {"Violet",0x14}
};
const char *rcr_swatch_name(int choice) {
    return swatches[(unsigned)choice < RCR_SWATCH_COUNT ? choice : 0].name;
}
uint8_t rcr_shirt_color(int owner, uint8_t original) {
    if (owner < 1 || owner > 2) return original;
    int choice = g_nes_config.outfit[owner-1];
    /* Keep guest palette flashes and fades. Only replace normal fabric. */
    if (choice > 0 && choice < RCR_SWATCH_COUNT && original == (owner == 1 ? 0x30 : 0x11))
        return swatches[choice].color;
    return original;
}
uint8_t rcr_sprite_color(uint8_t tag, uint8_t tile, int row, int bit, int pixel, const uint8_t palette[4]) {
    uint8_t original=palette[pixel]&63;
    if (saw_art_tag(tag))
        return (g_ram[0x3e]&2) ? original : pixel==3 ? 0x27 : pixel==2 ? 0x10 : 0x0f;
    if (tag>=HEAD_ART) {
        if (!g_nes_config.blood_effects || tag>=CUT_TOP || (g_ram[0x3e]&2)) return original;
        char c=head_ink(tag,row,bit);
        return c=='r' ? 0x06 : c=='s' ? 0x26 : c=='w' ? 0x30 : 0x0f;
    }
    if (g_nes_config.oni_mode && tag>>6) {
        /* Follow the guest subpalette during fades and damage flashes. */
        if ((palette[3]&63)!=0x26) return original;
        if (pixel==1 && original!=0x0f) return original;
        if (pixel==2 && original!=0x11 && original!=0x30) return original;
        if (oni_has_tile(tile)) {
            char ink=oni_ink(tile,row,bit);
            return oni_pen(ink)==(unsigned)pixel ? oni_color(ink,tag>>6,original) : original;
        }
        /* Dust clouds in the player animation table keep their guest palette. */
        return original;
    }
    if (pixel==2) {
        int pants=(tag>>4)&3;
        if (pants && pants<=2 && original==(pants==1 ? 0x11 : 0x30)) {
            int choice=g_nes_config.pants[pants-1];
            if (choice>0 && choice<RCR_SWATCH_COUNT) return swatches[choice].color;
        }
        return rcr_shirt_color(tag & 3,original);
    }
    int owner=(tag>>2)&3;
    if (pixel==1 && owner>=1 && owner<=2 && original==0x0f && (hair_region[tile][row] & (1u<<bit))) {
        int pose=crown_pose[tile];
        if (pose && rcr_eye_style(owner-1) && (sockets[pose-1][row] & (1u<<bit))) return original;
        int choice=g_nes_config.hair[owner-1];
        if (choice>0 && choice<RCR_SWATCH_COUNT) return swatches[choice].color;
    }
    return original;
}
uint32_t rcr_swatch_color(int player, int choice, int hair) {
    return g_nes_palette[choice > 0 && choice < RCR_SWATCH_COUNT ?
        swatches[choice].color : (hair ? 0x0f : player ? 0x11 : 0x30)];
}
/* Match the active player's decoded stream at the guest's tile or attribute
 * store. Attribute tables overlap in the ROM, so the tile source matters too. */
void rcr_oam_sprite_write(uint16_t pc, int slot) {
    shadow_tags[slot] = 0;
    int obj = g_ram[0x47];
    if ((pc != 0xeeaa && pc != 0xeee0 && pc != 0xeeeb) || obj >= OBJECTS || !gameplay()) return;
    const Packet *packet = &s_actors.shadow[obj];
    unsigned tile = (g_ram[0x2c] | (g_ram[0x2d] << 8)) + g_cpu.Y;
    unsigned attr = (g_ram[0x30] | (g_ram[0x31] << 8)) + g_cpu.Y;
    for (int i = 0; i < packet->count; ++i) {
        const Sprite *s = &packet->tiles[i];
        if ((s->regions || s->effect) && tile==s->tile_source && (s->attr_source ? attr==s->attr_source : !g_ram[0x31])) {
            shadow_tags[slot] = appearance_tag(s,obj);
            break;
        }
    }
}
void rcr_appearance_dma(uint8_t page, const uint8_t *oam, int repair) {
    for (int i=0; i<64; ++i) {
        uint8_t tag = page == 2 ? shadow_tags[i] : 0;
        if (page == 2 && g_nes_config.reduce_sprite_flicker) {
            if (repair && oam[4*i] == 0xf8) tag = coherent_tags[i];
            else coherent_tags[i] = tag;
        }
        display_tags[i] = tag;
    }
}
void rcr_native_sprite_tags(uint8_t tags[64]) { memcpy(tags,display_tags,64); }

static void finish_head_sprites(Packet *p) {
    int left=32767,right=-32768,top=32767,bottom=-32768,body_top=32767;
    for (int i=0;i<p->count;i++) {
        Sprite *s=&p->tiles[i];
        if (s->effect==MARK_BODY && s->y<body_top) body_top=s->y;
        if (s->effect!=MARK_HEAD && s->effect!=MARK_SAW) continue;
        if (s->x<left) left=s->x;
        if (s->x>right) right=s->x;
        if (s->y<top) top=s->y;
        if (s->y>bottom) bottom=s->y;
    }
    int pose=(p->source.anim&127)%63;
    for (int i=0;i<p->count;i++) {
        Sprite *s=&p->tiles[i];
        if (s->effect==MARK_SAW) {
            int vertical=bottom-top>right-left;
            int part=vertical ? (s->y>top) : (s->x>left);
            int mirror=(p->source.anim&128)!=0;
            s->effect=(!vertical && s->y<bottom) || (vertical && s->x>left) ? SAW_BLANK :
                SAW_ART+(part^mirror)+2*vertical+(((s->attr>>4)&12)^(mirror?(vertical?8:4):0));
        } else if (s->effect==MARK_HEAD) {
            int compact=bottom-top<5;
            int part=(2*s->x>left+right) | ((!compact && 2*s->y>top+bottom)<<1);
            s->effect=(compact?HEAD_COMPACT:HEAD_ART)+part+((s->attr>>4)&12);
        } else if (s->effect==MARK_BODY) {
            s->effect=0;
            if ((pose>=0x14 && pose<=0x16) || pose==0x3e) {
                /* The prone head occupies the far two columns. */
                int dx=s->x-p->source.x;
                if ((p->source.anim&128) ? dx<=-16 : dx>=8) s->effect=HIDDEN;
            } else if (pose>=0x18 && pose<=0x1b) {
                if (s->tile==0xa2 || s->tile==0xa7) s->effect=HIDDEN;
            } else if (!(pose>=0x1c && pose<=0x1f)) {
                int cut=body_top+12-s->y;
                if (cut>=8) s->effect=HIDDEN;
                else if (cut>0) s->effect=((s->attr&128)?CUT_BOTTOM:CUT_TOP)+cut-1;
            }
        }
    }
}

/* The heavy throwable uses different pose numbers for walking, jumping,
 * crouching and attacking. Select the corresponding hands-down pose for
 * the graphics lookup while combat still uses its original move and timing. */
static uint8_t saw_pose(uint8_t anim) {
    int pose=anim&127;
    if (pose==127) return anim;
    int page=pose>=63 ? 63 : 0;
    switch (pose-page) {
    case 3: case 5: pose=1; break;
    case 4: pose=0; break;
    case 6: pose=2; break;
    case 8: pose=0; break;
    case 9: pose=7; break;
    case 11: pose=10; break;
    case 13: pose=12; break;
    case 0x2c: pose=1; break;     /* draw back */
    case 0x2d: pose=0x2a; break;  /* extend the arms */
    default: return anim;
    }
    return (anim&128)|(page+pose);
}

enum { HAND_WEAPON_KIND=4 };
uint8_t rcr_saw_graphics_read(uint16_t pc,uint16_t addr,uint8_t value) {
    int obj=g_ram[0x47];
    if (obj>=4 || !wielding_saw(obj)) return value;
    if (pc==0xed3b) return addr==0xd3+obj ? saw_pose(value) : value;
    if (pc==0xee4f) return addr==0x3b0+obj ? HAND_WEAPON_KIND : value;
    const Packet *p=&s_actors.shadow[obj];
    unsigned index=g_cpu.Y/2;
    unsigned tile=(g_ram[0x2c]|(g_ram[0x2d]<<8))+index;
    for (int part=0;part<2;++part) {
        if (p->saw_grip[part].offsets && p->saw_grip[part].tile==tile &&
            addr==p->saw_grip[part].offsets+(pc==0xef09))
            return pc==0xeef8 ? p->saw_grip[part].x : p->saw_grip[part].y;
    }
    return value;
}

/* Decode ED0E..EF88's metasprite bytecode, including its linked streams,
 * per-character palettes, held objects and alternate face tiles. Signed world
 * coordinates replace only the final 8-bit screen clip. Guest RAM, OAM and
 * timing are untouched. Packets are promoted at the same DMA as native OAM. */
static void decode_packet(Packet *out) {
    const Source *src=&out->source;
    out->count=0; out->room=src->room; out->order=src->order;
    memset(out->saw_grip,0,sizeof out->saw_grip);
    if (!src->valid) return;
    int held_saw=src->item_art==SAW_HELD;
    int type=src->type, anim=held_saw ? saw_pose(src->anim) : src->anim;
    int saw_parts=0;
    if ((anim&0x7f)==0x7f || type>=0x40) return;
    unsigned table=word(3,0x8000+type*2), ptr=word(3,table+(anim&0x7f)*2);
    unsigned back=0, back_index=0, index=0;
    int flip=(anim&0x80)!=0;
    int wy=src->wy;
    int carried_owner=0, attachment=0;
    unsigned carried_alex_attr=0, carried_ryan_attr=0;
    for (int guard=0; guard<512; guard++) {
        if (ptr<0x8000 || ptr>0xbfff || index>255) break;
        int count=prg(3,ptr+index++), attr=0;
        unsigned attr_ptr=0, alex_attr=0, ryan_attr=0;
        if (!count) {
            if (src->item_art || src->headless) finish_head_sprites(out);
            return;
        }
        if (count==0xff) {
            unsigned next=word(3,ptr+index); index+=2;
            back=ptr; back_index=index; ptr=next; index=0; continue;
        }
        if (count==0xfc) {
            if ((src->flags&0x40) &&
                (src->held_by>=4 || (src->held_state&0x7f)==0x15)) {
                unsigned list=ptr+index;
                attachment=1;
                back=word(3,list); back_index=0;
                /* EE4F..EE58 subtracts five ONCE, not modulo five.
                 * In particular, held enemy kind 10 selects entry 5. */
                unsigned kind=held_saw ? HAND_WEAPON_KIND : src->held_kind;
                if (kind>=5) kind-=5;
                ptr=word(3,list+2+2*kind);
                /* Slots 3 and 4 in the $FC branch table are the two player
                 * versions of the carried body. Their $FE attribute lists
                 * identify its shirt independently of the carrier's outfit. */
                if (src->held_by < 2 && (kind == 3 || kind == 4)) {
                    unsigned alex=word(3,list+2+2*3), ryan=word(3,list+2+2*4);
                    if (prg(3,alex)==0xfe && prg(3,ryan)==0xfe) {
                        carried_owner=src->held_by+1;
                        carried_alex_attr=word(3,alex+2);
                        carried_ryan_attr=word(3,ryan+2);
                    }
                }
            } else ptr=word(3,ptr+index);
            index=0; continue;
        }
        if (count==0xfd) {
            count=prg(3,ptr+index++);
            unsigned list=ptr+index;
            int variant=src->variant;
            if (variant>2) variant=2;
            alex_attr=word(3,list+2); ryan_attr=word(3,list+4);
            attr_ptr=word(3,list+2+variant*2);
            ptr=word(3,list); index=0;
        } else if (count==0xfe) {
            count=prg(3,ptr+index++);
            attr_ptr=word(3,ptr+index); index+=2;
        } else attr=prg(3,ptr+index++);
        unsigned tiles=word(3,ptr+index), offsets=word(3,ptr+index+2); index+=4;
        if (attr&0x80) attr=prg(7,0xef89+type);
        for (int n=0; n<count; n++) {
            if (out->count==TILES_PER_OBJECT) { s_decode_errors++; return; }
            int tile=prg(3,tiles+n);
            if (tile>=0x60 && tile<0x68 && type<8)
                tile=prg(7,0xef99+(tile-0x60)*5+src->variant);
            int dx=(int8_t)prg(3,offsets+n*2), dy=(int8_t)prg(3,offsets+n*2+1);
            Sprite *s=&out->tiles[out->count++];
            int grip_part=-1;
            if (held_saw && attachment && saw_parts<2) {
                grip_part=saw_parts++;
                int pose=(anim&127)%63;
                /* In unflipped ROM space the fighter faces left. The motor
                 * sits over the forward fist, then follows its extension. */
                int reach=5;
                if ((src->anim&127)%63==0x2c) reach=2;
                if (pose==0x2a) reach=18;
                dx=-reach-8*grip_part;
                dy=pose==12 ? 7 : pose==0 ? 14 : 15;
                if (pose==0x2a) dy=17+grip_part;
                out->saw_grip[grip_part].tile=tiles+n;
                out->saw_grip[grip_part].offsets=offsets+2*n;
                out->saw_grip[grip_part].x=dx;
                out->saw_grip[grip_part].y=dy;
            }
            s->x=(int16_t)(src->x+(flip ? -dx-1 : dx)-4);
            s->y=(int16_t)(237-wy-dy);
            /* $FD swaps subpalettes 0 -> 1 on the shirt, and 1 -> 0 on
             * trousers. Face and held-item streams keep their own attributes. */
            if (carried_owner) {
                alex_attr=carried_alex_attr;
                ryan_attr=carried_ryan_attr;
            }
            s->carried_owner=(uint8_t)carried_owner;
            int body=carried_owner || (!attachment && type==0);
            int cloth=0;
            if (body && alex_attr && ryan_attr) {
                unsigned alex=prg(3,alex_attr+n)&3, ryan=prg(3,ryan_attr+n)&3;
                if (alex==0 && ryan==1) cloth=SHIRT;
                if (alex==1 && ryan==0) cloth=PANTS;
            }
            static const uint8_t empty_region[8]={0};
            int hair=body &&
                     (crown_pose[tile] || memcmp(hair_region[tile],empty_region,sizeof empty_region));
            s->regions=cloth | (hair ? HAIR : 0) | (body ? BODY : 0);
            s->effect=held_saw && attachment ? SAW_BLANK :
                      src->item_art==SAW_LOOSE ? MARK_SAW :
                      (src->item_art==HEAD_LOOSE || (src->item_art==HEAD_HELD && attachment)) ? MARK_HEAD :
                      (src->headless && !attachment) ? MARK_BODY : 0;
            s->tile_source=tiles+n;
            s->attr_source=attr_ptr ? attr_ptr+n : 0;
            s->tile=(uint8_t)tile;
            s->attr=(uint8_t)((attr_ptr ? prg(3,attr_ptr+n) : attr)^(flip?0x40:0));
            if (grip_part>=0)
                s->effect=HELD_SAW_ART+grip_part+(((s->attr>>5)&6)^(flip?0:2));
        }
        if (back) { ptr=back; index=back_index; back=0; carried_owner=0; attachment=0; }
    }
    out->count=0; s_decode_errors++;
}
static void capture_object(int obj, uint32_t order) {
    if (obj<0 || obj>=OBJECTS) return;
    Source *src=&s_actors.shadow[obj].source;
    memset(src,0,sizeof *src);
    src->x=(int16_t)world_x(obj);
    src->wy=(int16_t)((g_ram[0x9e + obj] | (g_ram[0xa7+obj]<<8))+
                      (g_ram[0xb9+obj] | (g_ram[0xc2+obj]<<8)));
    src->order=order;
    src->type=g_ram[0xcb+obj]; src->anim=g_ram[0xd3+obj];
    src->variant=g_ram[0x48+obj]&0x7f; src->flags=g_ram[0x56+obj];
    src->held_by=g_ram[0x3b7+obj]; src->held_kind=g_ram[0x3b0+obj];
    if (src->held_by<OBJECTS) src->held_state=g_ram[0x4f+src->held_by];
    src->headless=(headless_fighters>>obj)&1;
    if (has_saw(obj)) src->item_art=SAW_LOOSE;
    else if (obj<4 && wielding_saw(obj)) src->item_art=SAW_HELD;
    else if (head_items&(1u<<obj)) src->item_art=HEAD_LOOSE;
    else if ((src->flags&0x40) && src->held_by<OBJECTS && (head_items&(1u<<src->held_by)))
        src->item_art=HEAD_HELD;
    src->room=g_ram[0x42]; src->valid=1;
    decode_packet(&s_actors.shadow[obj]);
}
static void clear_shadow_list(void) {
    s_actors.shadow_camera=camera();
    memset(s_actors.shadow,0,sizeof s_actors.shadow);
    s_list_pending=0;
}
static int observe_sprite(uint16_t addr) {
    (void)addr;
    if (gameplay() && !pause_panel()) {
        int first = s_list_pending;
        if (first) clear_shadow_list();
        capture_object(g_ram[0x47], ++s_actors.sequence);
        if (first) {
            /* F046..F04B visits the sorted $069D list backwards. DMA can
             * interrupt between calls. Decode the remaining current poses now,
             * in the exact order their later ED0E calls will receive. */
            int remaining = g_ram[0x6b5];
            if (remaining < OBJECTS) {
                uint32_t order = s_actors.sequence;
                for (int i = remaining - 1; i >= 0; --i)
                    capture_object(g_ram[0x69d + i], ++order);
            }
        }
    }
    return 0;
}
static int suppress_sprite(int slot, int x, int y, void *user) {
    (void)slot; (void)x; (void)user;
    return s_enabled && live_terrain.pages>0 && s_room==g_ram[0x42] &&
           ppu_renderer_in_custom_native_pass() && gameplay() &&
           !pause_panel() &&
           (!g_ram[0x3f] || (y>=32 && y<192));
}
void rcr_ws_dma(void) {
    if (!s_ready || pause_panel()) return;
    if (s_actors.room != g_ram[0x42]) {
        memset(&s_actors,0,sizeof s_actors); s_actors.room=g_ram[0x42];
        s_actors.shadow_camera=s_actors.present_camera=camera();
        s_list_pending=0;
    }
    s_actors.present_camera=s_actors.shadow_camera;
    for (int obj=0; obj<OBJECTS; obj++) {
        if (!(g_ram[0x48+obj]&0x80)) {
            s_actors.shadow[obj].count=0; s_actors.shadow[obj].source.valid=0;
        }
        s_actors.present[obj]=s_actors.shadow[obj];
    }
}
static void draw_packet(uint32_t *out, int width, const Packet *packet, int owner,int view,int native) {
    if (packet->room != g_ram[0x42]) return;
    for (int n=0; n<packet->count; n++) {
        const Sprite *s=&packet->tiles[n];
        uint8_t tag=appearance_tag(s,owner-1);
        for (int y=0; y<8; y++) {
            int py=s->y+y;
            if (py<32 || py>=192) continue;
            int row=(s->attr&0x80)?7-y:y;
            int offset=0x1000+s->tile*16+row;
            uint8_t lo=g_chr_ram[offset], hi=g_chr_ram[offset+8];
            if (tag) rcr_style_sprite_row(tag,s->tile,row,&lo,&hi);
            for (int x=0; x<8; x++) {
                int px=s->x-view+x;
                if (px<0 || px>=width) continue;
                int bit=(s->attr&0x40)?x:7-x;
                int c=((lo>>bit)&1)|(((hi>>bit)&1)<<1);
                if (!c || ((s->attr&0x20)&&(native?ppu_renderer_background_opaque(px,py):s_opaque[py*width+px]))) continue;
                const uint8_t *palette=g_ppu_pal+0x10+(s->attr&3)*4;
                uint8_t color=tag ? rcr_sprite_color(tag,s->tile,row,bit,c,palette) : palette[c]&63;
                out[py*width+px]=g_nes_palette[color];
            }
        }
    }
}
/* Record the enemy immediately before the guest writes its coin type.
 * The copy lives outside actor RAM, so the coin and subsequent spawns
 * keep the slot. Render the settled knockdown pose at street depth. */
enum { BODY_LIMIT=48 };
typedef struct {
    int16_t x,lane;
    uint8_t room,type,variant,anim,headless,cut; /* cut occupies legacy padding */
} FallenEnemy;
typedef struct {
    FallenEnemy body[BODY_LIMIT];
    uint8_t count,next;
} BodyState;
static BodyState bodies;
static Packet body_packets[BODY_LIMIT];

static void decode_body(int index) {
    const FallenEnemy *b=&bodies.body[index];
    Packet *p=&body_packets[index];
    p->source=(Source){.valid=1,.type=b->type,.variant=b->variant,.anim=b->anim,
        .room=b->room,.x=b->x,.wy=b->lane,.headless=b->headless};
    decode_packet(p);
}

void rcr_enemy_coin(int slot) {
    if (!g_nes_config.persistent_bodies || !gameplay() || slot<2 || slot>3) return;
    /* $AFC8 has already replaced the RAM pose with the spinning coin.
     * The previous sprite list still carries the fighter's orientation. */
    const Source *last=&s_actors.present[slot].source;
    unsigned facing=(last->valid && last->room==g_ram[0x42] && last->type==g_ram[0xcb+slot])
        ? last->anim : g_ram[0x6f+slot];
    int index=bodies.next;
    bodies.body[index]=(FallenEnemy){.x=world_x(slot),.lane=g_ram[0x9e + slot]|(g_ram[0xa7+slot]<<8),
        .room=g_ram[0x42],.type=g_ram[0xcb+slot],.variant=g_ram[0x48+slot]&127,
        .anim=0x14|(facing&128),.headless=(headless_fighters>>slot)&1};
    decode_body(index);
    bodies.next=(index+1)%BODY_LIMIT;
    if (bodies.count<BODY_LIMIT) ++bodies.count;
}

static int bodies_get(uint8_t *buf,int cap) {
    if (cap<(int)sizeof bodies+1) return -1;
    memcpy(buf,&bodies,sizeof bodies);
    buf[sizeof bodies]=1;
    return sizeof bodies+1;
}

static int bodies_set(const uint8_t *buf,int len) {
    int legacy=len==sizeof bodies;
    if (!legacy && (len!=sizeof bodies+1 || buf[sizeof bodies]!=1)) return 0;
    BodyState saved;
    memcpy(&saved,buf,sizeof saved);
    if (saved.count>BODY_LIMIT || saved.next>=BODY_LIMIT ||
        (saved.count<BODY_LIMIT && saved.next!=saved.count)) return 0;
    for (int i=0;i<saved.count;++i) {
        FallenEnemy *b=&saved.body[i];
        if (legacy) b->cut=0;
        if (b->cut>1 || b->type>=8 || b->room>=ROOMS || (b->anim&127)!=0x14) return 0;
    }
    bodies=saved;
    for (int i=0;i<bodies.count;++i) decode_body(i);
    return 1;
}
_Static_assert(sizeof(BodyState)+1<=NES_MOD_SAVESTATE_BLOB_CAP,"body save record size");

static void cut_bodies(void) {
    if (!g_nes_config.blood_effects || !g_nes_config.persistent_bodies ||
        !gameplay() || pause_panel() || (g_ram[0x3e]&2)) return;
    for (int fighter=0;fighter<4;++fighter) {
        int move=g_ram[0x4f+fighter]&127;
        if (!(g_ram[0x48+fighter]&128) || !wielding_saw(fighter) ||
            (move!=0x20 && move!=0x21 && move!=0x22 && move!=0x23 && move!=0x29 && move!=0x2a) ||
            g_ram[0xb9+fighter]>16 || g_ram[0xc2+fighter]) continue;
        int direction=(g_ram[0x6f+fighter]&128) ? -1 : 1;
        int lane=g_ram[0x9e + fighter]|(g_ram[0xa7+fighter]<<8);
        for (int i=0;i<bodies.count;++i) {
            FallenEnemy *b=&bodies.body[i];
            int ahead=(b->x-world_x(fighter))*direction;
            if (b->cut || b->room!=g_ram[0x42] || ahead < -10 || ahead>32 || abs(b->lane-lane)>10) continue;
            b->cut=1;
            blood_enter_room(b->room);
            spray(b->x,240-b->lane,-12,direction,SAW_SPRAY,1,1);
        }
    }
}

static void draw_bodies(uint32_t *out,int width,int view,int native) {
    if (!g_nes_config.persistent_bodies || !gameplay() || pause_panel() || (g_ram[0x3e]&2)) return;
    for (int i=0;i<bodies.count;++i) {
        int index=(bodies.next+BODY_LIMIT-bodies.count+i)%BODY_LIMIT;
        const Packet *p=&body_packets[index];
        if (bodies.body[index].cut && g_nes_config.blood_effects && p->room==g_ram[0x42]) {
            Packet pieces=*p;
            /* The prone pose has four tile columns. Separate the two
             * ends from the torso while preserving each part's artwork. */
            for (int j=0;j<pieces.count;++j) {
                Sprite *tile=&pieces.tiles[j];
                int dx=tile->x-p->source.x;
                if (dx < -8) { tile->x-=8; tile->y+=3; }
                else if (dx>=8) { tile->x+=10; tile->y-=3; }
            }
            draw_packet(out,width,&pieces,3,view,native);
        } else draw_packet(out,width,p,3,view,native);
    }
}
static void draw_street_items(uint32_t *out,int width,int view,int native) {
    if (street_room!=g_ram[0x42] || !gameplay() || pause_panel() || (g_ram[0x3e]&2)) return;
    for (int i=0;i<STREET_SLOTS;++i) {
        StreetWeapon *item=&street_items[i];
        if (!item->type) continue;
        Packet packet;
        packet.source=(Source){.valid=1,.type=item->type,.anim=item->anim,
            .room=street_room,.variant=item->variant,.x=item->x,.wy=item->lane+item->height,
            .item_art=item->kind==ITEM_SAW ? SAW_LOOSE : item->kind};
        decode_packet(&packet);
        draw_packet(out,width,&packet,5,view,native);
    }
}
static void draw_actors(uint32_t *out, int width) {
    draw_bodies(out,width,s_view,0);
    draw_street_items(out,width,s_view,0);
    int order[OBJECTS];
    for (int i=0; i<OBJECTS; i++) order[i]=i;
    for (int i=0; i<OBJECTS; i++) for (int j=i+1; j<OBJECTS; j++)
        if (s_actors.present[order[j]].order<s_actors.present[order[i]].order) {
            int t=order[i]; order[i]=order[j]; order[j]=t;
        }
    for (int i=0; i<OBJECTS; i++) draw_packet(out,width,&s_actors.present[order[i]],order[i]+1,s_view,0);
}
static int render(uint32_t *out, int width, int height, int native_x0,
                  const uint32_t *native, void *user) {
    (void)height; (void)user;
    if (!s_enabled || !s_ready) return 0;
    if (pause_panel()) {
        /* Full pause submenus replace the world with a flat panel. Extend
         * its blank right border across the viewport; keep native text and
         * cursor pixels centered at their original size. */
        for (int y=0; y<240; y++) {
            for (int x=0; x<width; x++) out[y*width+x]=native[y*256+255];
            memcpy(out+y*width+native_x0,native+y*256,256*sizeof(uint32_t));
        }
        s_wide_frames++;
        return 1;
    }
    if (!gameplay()) return 0;
    if (s_room!=g_ram[0x42]) decode_room();
    if (!live_terrain.pages) return 0;
    /* Native pixels use this frame's scroll. The wide viewport follows the
     * camera sampled with the displayed actor list, including repeated DMAs. */
    s_camera=native_camera(); s_view=view_left(s_actors.present_camera,width);
    for (int y=0; y<240; y++) for (int x=0; x<width; x++) {
        int opaque;
        out[y*width+x]=bg_pixel(s_view+x,y,&opaque);
        s_opaque[y*width+x]=(uint8_t)opaque;
    }
    int play_x0=s_camera-s_view;
    s_terrain_pixels=s_terrain_diffs=0;
    if (!(g_ram[0x3e]&2) && !g_ram[0x3f] && !g_ram[0x36])
        for (int y=32; y<192; y++) for (int x=8; x<248; x++) {
            int opaque; uint32_t color=bg_pixel(s_camera+x,y,&opaque);
            s_terrain_pixels++;
            if (color!=native[y*256+x]) s_terrain_diffs++;
        }
    /* Native background remains authoritative for live door/text edits and
     * split-screen scroll. HUD and pause/menu panels stay screen centered. */
    /* The NES's left-eight-pixel background mask is a hardware edge, not a
     * seam in the world. Keep the decoded background there inside a wide view. */
    for (int y=32; y<192; y++) for (int x=8; x<256; x++) {
        int px=play_x0+x;
        if (px>=0 && px<width) {
            out[y*width+px]=native[y*256+x];
            s_opaque[y*width+px]=(uint8_t)ppu_renderer_background_opaque(x,y);
        }
    }
    draw_blood(out,width,s_view);
    draw_actors(out, width);
    for (int y=0; y<32; y++)
        memcpy(out+y*width+native_x0,native+y*256,256*sizeof(uint32_t));
    for (int y=192; y<240; y++)
        memcpy(out+y*width+native_x0,native+y*256,256*sizeof(uint32_t));
    s_wide_frames++;
    return 1;
}

uint8_t rcr_ws_read(uint16_t pc, uint16_t addr, uint8_t value) {
    if (addr == 0x6b5 && pc == 0xefd9) {
        /* The previous OAM wait has finished. An empty list has no ED0E
         * visit. A nonempty one keeps the previous complete list during sort. */
        if (gameplay() && !pause_panel()) {
            if (value) s_list_pending = 1;
            else clear_shadow_list();
        }
        return value;
    }
    if (!s_enabled || !gameplay() || g_render_width<=256 || (addr!=0xdc && addr!=0xdd)) return value;
    if (s_room!=g_ram[0x42]) decode_room();
    if (!live_terrain.pages) return value;
    int origin=0,handled=0;
    if (g_current_bank==4 && (pc==0x82d6 || pc==0x82de)) {
        /* Hitboxes are translated into a common local frame per object pair,
         * so their 16-bit world positions work outside the old screen too.
         * The original boxes, attack rules and collision tests still run. */
        int obj=g_ram[0x1a];
        if (obj>=OBJECTS) return value;
        origin=world_x(obj)-64;
        /* The guest rejects an unsigned subtraction borrow before accepting
         * its local X. A negative origin wraps to $FFFF and clips Alex at
         * the room's left edge despite an otherwise valid local position. */
        if (origin<0) origin=0;
        handled=1; s_collision_reads++;
    } else if (g_current_bank==6 && (pc==0x9682 || pc==0x9686)) {
        /* The retreat routine frees the enemy slot if X - origin has a
         * nonzero high byte. Give it a 256-pixel window within the visible
         * street, sliding with the enemy until the window reaches an edge. */
        int slot=g_cpu.X;
        if (slot!=2 && slot!=3) return value;
        int left=view_left(camera(),g_render_width);
        int right=left+g_render_width;
        if (left<0) left=0;
        if (right>live_terrain.pages*256) right=live_terrain.pages*256;
        origin=world_x(slot);
        if (origin<left) origin=left;
        if (origin>right-256) origin=right-256;
        handled=1;
    } else if (g_current_bank==4 && (pc==0xb4af || pc==0xb4b5 || pc==0xbc19 || pc==0xbc1f)) {
        int obj=g_cpu.X;
        if (obj>=OBJECTS) return value;
        int wx=world_x(obj), left=view_left(camera(),g_render_width);
        origin=wx-((wx>=left+16 && wx<left+g_render_width-16)?128:512);
        handled=1;
        if (pc==0xb4af || pc==0xb4b5) s_activation_reads++; else s_spawn_reads++;
    }
    if (!handled) return value;
    return (uint8_t)((uint16_t)origin>>(addr==0xdd?8:0));
}
static void apply(void) {
    if (!s_ready) return;
    nes_video_set_aspect_mode(s_enabled?s_aspect:NES_ASPECT_STOCK);
    ppu_renderer_set_custom_render(s_enabled?render:NULL,NULL);
}
int rcr_ws_arg(const char *key, const char *value) {
    if (!strcmp(key,"--spawn-chainsaw")) { debug_saw_pending=1; return 1; }
    if (strcmp(key,"--widescreen") || !value) return 0;
    NesAspectMode mode;
    if (!nes_video_aspect_from_name(value,&mode)) return 0;
    s_enabled=mode!=NES_ASPECT_STOCK;
    if (s_enabled) s_aspect=mode;
    apply(); return 1;
}
void rcr_ws_frame(void) {
    if (!g_ram[0x40]) { bodies.count=bodies.next=0; opening_saw=0; }
    if (debug_saw_pending && spawn_saw(0)) {
        debug_saw_pending=0;
        if (!g_ram[0x42]) opening_saw=1;
    } else if (!opening_saw && !g_ram[0x42] && spawn_saw(32)) opening_saw=1;
    cut_bodies();
    street_step();
    blood_step();
    if (s_ready && s_room!=g_ram[0x42] && g_ram[0x42]<ROOMS) {
        decode_room();
        memset(&s_actors,0,sizeof s_actors); s_actors.room=g_ram[0x42];
        s_actors.shadow_camera=s_actors.present_camera=camera();
        s_list_pending=0;
    }
}
typedef struct {
    Source shadow[OBJECTS],present[OBJECTS];
    uint32_t sequence,room,list_pending;
    int16_t shadow_camera,present_camera;
    uint8_t head_items,headless,saws,opening_saw;
    StreetWeapon street[STREET_SLOTS];
    int32_t street_room;
    uint32_t blood_game;
} ActorSave;
_Static_assert(sizeof(ActorSave)<=NES_MOD_SAVESTATE_BLOB_CAP,"actor state exceeds save record");
static int state_get(uint8_t *buf, int cap) {
    if (cap<(int)sizeof(ActorSave)) return -1;
    ActorSave state={0};
    reserve_blood_game(); /* Even a clean save may acquire stains later. */
    state.blood_game=blood_game;
    memcpy(state.street,street_items,sizeof street_items); state.street_room=street_room;
    state.saws=saw_items; state.opening_saw=opening_saw;
    state.head_items=head_items; state.headless=headless_fighters;
    state.shadow_camera=s_actors.shadow_camera; state.present_camera=s_actors.present_camera;
    state.sequence=s_actors.sequence; state.room=s_actors.room;
    state.list_pending=s_list_pending;
    for (int i=0;i<OBJECTS;i++) {
        state.shadow[i]=s_actors.shadow[i].source; state.present[i]=s_actors.present[i].source;
    }
    memcpy(buf,&state,sizeof state); return sizeof state;
}
static int state_set(const uint8_t *buf, int len) {
    /* v0.1.6 states have the same source packets but no pending-list field. */
    if (len!=sizeof(ActorSave) && len!=offsetof(ActorSave,blood_game) && len!=offsetof(ActorSave,street) && len!=offsetof(ActorSave,head_items) && len!=offsetof(ActorSave,shadow_camera) && len!=offsetof(ActorSave,list_pending)) return 0;
    ActorSave state={0}; memcpy(&state,buf,(size_t)len);
    if (state.list_pending>1) return 0;
    if (state.room>=ROOMS) return 0;
    for (int i=0;i<OBJECTS;i++) {
        if (state.shadow[i].valid>1 || state.present[i].valid>1 ||
            state.shadow[i].type>=0x40 || state.present[i].type>=0x40) return 0;
    }
    int has_street=len>=(int)offsetof(ActorSave,blood_game);
    if (has_street) {
        if (state.street_room < -1 || state.street_room >= 256) return 0;
        for (int i=0;i<STREET_SLOTS;++i) {
            StreetWeapon w=state.street[i];
            if (w.type && (w.type<8 || w.type>15 || w.kind>ITEM_SAW)) return 0;
        }
    }
    bodies.count=bodies.next=0; /* Older saves have no subsequent body record. */
    memcpy(street_items,state.street,sizeof street_items);
    street_room=has_street ? state.street_room : -1;
    saw_items=state.saws&0x70; opening_saw=state.opening_saw!=0;
    head_items=state.head_items&0x70;
    headless_fighters=state.headless&0x0c;
    if (len<(int)offsetof(ActorSave,street)) for (int i=0;i<OBJECTS;++i) {
        state.shadow[i].item_art=state.present[i].item_art=0;
        state.shadow[i].headless=state.present[i].headless=0;
    }
    select_blood_game(state.blood_game);
    memset(&s_actors,0,sizeof s_actors);
    s_actors.sequence=state.sequence; s_actors.room=(uint8_t)state.room;
    s_list_pending=state.list_pending;
    int saved_camera=len>=(int)offsetof(ActorSave,head_items);
    s_actors.shadow_camera=saved_camera ? state.shadow_camera : camera();
    s_actors.present_camera=saved_camera ? state.present_camera : camera();
    for (int i=0;i<OBJECTS;i++) {
        s_actors.shadow[i].source=state.shadow[i]; s_actors.present[i].source=state.present[i];
        decode_packet(&s_actors.shadow[i]); decode_packet(&s_actors.present[i]);
    }
    /* Legacy states predate the native slot tags. Match full tile records
     * to recover visible ownership until the next OAM rebuild. */
    memset(display_tags,0,64);
    memset(shadow_tags,0,64);
    for (int i=0;i<64;++i) for(int obj=0;obj<OBJECTS;++obj) {
        Packet *packets[2]={&s_actors.present[obj],&s_actors.shadow[obj]};
        for(int k=0;k<2;++k) {
            const uint8_t *oam=(k?g_ram+0x200:g_ppu_oam)+4*i;
            for(int n=0;n<packets[k]->count;++n) {
                Sprite *t=&packets[k]->tiles[n];
                if(appearance_tag(t,obj) && t->tile==oam[1] && t->attr==oam[2] &&
                   t->x-camera()==oam[3] && t->y==oam[0]+1)
                    (k?shadow_tags:display_tags)[i]=appearance_tag(t,obj);
            }
        }
    }
    memcpy(coherent_tags,display_tags,64);
    /* Terrain is derived from the ROM; rebuild without clearing restored actors. */
    decode_room(); return 1;
}
static int shirt_state_get(uint8_t *buf, int cap) {
    if (cap < 193) return -1;
    buf[192]=3;
    memcpy(buf,shadow_tags,64); memcpy(buf+64,display_tags,64); memcpy(buf+128,coherent_tags,64);
    return 193;
}
static int shirt_state_set(const uint8_t *buf, int size) {
    if (size==192 || (size==193 && (buf[192]==1 || buf[192]==2))) {
        /* Retain newly reconstructed regions missing from the older tag format. */
        int version=size==192 ? 0 : buf[192];
        for (int i=0;i<64;i++) {
            uint8_t *tags[]={shadow_tags,display_tags,coherent_tags};
            for (int k=0;k<3;k++) {
                uint8_t saved=buf[k*64+i];
                /* Some formerly untagged tiles now carry the lower eyes. */
                int mask=version==2 ? 63 : version==1 && (saved&12) ? 15 : 3;
                uint8_t tag=(tags[k][i]&~mask)|(saved&mask);
                /* A repaired slot can be hidden in OAM at load time.
                 * Its saved clothing or face tag still identifies its owner. */
                unsigned region=saved & mask;
                unsigned owner=(region | (region>>2) | (region>>4)) & 3;
                if (owner==1 || owner==2) tag=(tag&63) | owner*BODY;
                tags[k][i]=tag;
            }
        }
        return 1;
    }
    if (size!=193 || buf[192]!=3) return 0;
    memcpy(shadow_tags,buf,64); memcpy(display_tags,buf+64,64); memcpy(coherent_tags,buf+128,64);
    return 1;
}
void rcr_ws_init(const char *rom_path) {
    FILE *f=rom_path?fopen(rom_path,"rb"):NULL;
    if (!f) { fprintf(stderr,"[Widescreen] Cannot read owner ROM\n"); return; }
    int ok=fseek(f,16,SEEK_SET)==0 && fread(s_rom,1,sizeof s_rom,f)==sizeof s_rom;
    fclose(f);
    if (!ok) { fprintf(stderr,"[Widescreen] Incomplete owner ROM\n"); return; }
    nesrecomp_exe_dir(blood_dir,sizeof blood_dir);
    load_faces();
    s_ready=1;
    nes_video_on_window_resized(1280,720);
    nes_mod_register_function_entry_plugin("rcr.widescreen.metasprite",0xed0e,observe_sprite);
    /* Native-width play also supplies actors for the frozen wide preview. */
    nes_mod_set_function_hook_enabled("rcr.widescreen.metasprite",1);
    nes_mod_register_savestate_hook("rcr.widescreen.actors",state_get,state_set);
    nes_mod_register_savestate_hook("rcr.bodies",bodies_get,bodies_set);
    nes_mod_register_savestate_hook("rcr.shirt-owners",shirt_state_get,shirt_state_set);
    ppu_renderer_set_sprite_suppress(suppress_sprite,NULL);
    apply();
    fprintf(stderr,"[Widescreen] Experimental enhanced world renderer: %s\n",s_enabled?nes_video_aspect_name(s_aspect):"off");
}
int rcr_ws_debug(const char *cmd, int id, const char *json) {
    (void)json;
    int w,h;
    if (sscanf(cmd,"rcr_window_%dx%d",&w,&h)==2 && w>=256 && w<=7680 && h>=240 && h<=4320) {
        SDL_Window *window=SDL_GetWindowFromID(1);
        if (window) SDL_SetWindowSize(window,w,h);
        else nes_video_on_window_resized(w,h);
        debug_server_send_fmt("{\"id\":%d,\"ok\":true,\"window\":%s}",id,window?"true":"false"); return 1;
    }
    if (!strncmp(cmd,"rcr_aspect_",11)) {
        if (!rcr_ws_arg("--widescreen",cmd+11)) return 0;
        debug_server_send_fmt("{\"id\":%d,\"ok\":true}",id); return 1;
    }
    if (!strcmp(cmd,"rcr_spawn_chainsaw")) {
        if (spawn_saw(0)) debug_server_send_fmt("{\"id\":%d,\"ok\":true}",id);
        else debug_server_send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"Requires local street gameplay and an available item slot\"}",id);
        return 1;
    }
    if (strcmp(cmd,"rcr_widescreen")) return 0;
    debug_server_send_fmt("{\"id\":%d,\"ok\":true,\"width\":%d,\"room\":%d,\"pages\":%d,\"camera\":%d,\"view\":%d,\"wide_frames\":%llu,\"decode_errors\":%llu,\"collision_reads\":%llu,\"activation_reads\":%llu,\"spawn_reads\":%llu,\"sprites\":[%u,%u,%u,%u,%u,%u,%u],\"terrain_pixels\":%u,\"terrain_diffs\":%u,\"pause_panel\":%u}",
        id,g_render_width,s_room,live_terrain.pages,s_camera,s_view,(unsigned long long)s_wide_frames,
        (unsigned long long)s_decode_errors,(unsigned long long)s_collision_reads,
        (unsigned long long)s_activation_reads,(unsigned long long)s_spawn_reads,
        s_actors.present[0].count,s_actors.present[1].count,s_actors.present[2].count,
        s_actors.present[3].count,s_actors.present[4].count,s_actors.present[5].count,s_actors.present[6].count,
        s_terrain_pixels,s_terrain_diffs,(unsigned)pause_panel());
    return 1;
}

/* Keep the guest's name and stamina pixels together with its live wallet.
 * The native HUD occupies rows 16..23. Its following tile row is available
 * for totals when a narrow Fit window cannot fit both totals inline. */
enum { HUD_Y = 16, HUD_ROWS = 16, WALLET_SPACE = 64 };
static uint32_t native_hud[HUD_ROWS][256];
static unsigned wallet_players;

static void draw_wallet_hud(uint32_t *out, int width) {
    if (!wallet_players) return;
    int origin = (width - 256) / 2;
    int gap = width >= 256 + 2 * WALLET_SPACE ? WALLET_SPACE : 0;
    for (int row = 0; row < HUD_ROWS; ++row) {
        uint32_t *line = out + (HUD_Y + row) * width;
        const uint32_t *native = native_hud[row];
        for (int x = 0; x < width; ++x) line[x] = native[0];
        if (gap && row < 8) {
            /* Insert a field after each name and before its stamina bar. */
            memcpy(line + origin - gap, native, 64 * sizeof *line);
            memcpy(line + origin + 64, native + 64, 112 * sizeof *line);
            memcpy(line + origin + 176 + gap, native + 176, 80 * sizeof *line);
        } else {
            memcpy(line + origin, native, 256 * sizeof *line);
        }
    }
    if (width == 256) return;
    for (int player = 0; player < 2; ++player) {
        if (!(wallet_players & (1u << player))) continue;
        /* Bank 0 $BAFE..$BB64 prints three packed-BCD dollars digits and
         * two cents digits. P2's wallet starts three bytes after P1's. */
        const uint8_t *wallet = g_ram + 0x4c7 + player * 3;
        unsigned dollars = (wallet[2] & 15) * 100 + (wallet[1] >> 4) * 10 + (wallet[1] & 15);
        unsigned cents = (wallet[0] >> 4) * 10 + (wallet[0] & 15);
        char amount[12];
        snprintf(amount, sizeof amount, "$%u.%02u", dollars, cents);
        int x = origin + (gap ? 64 - gap : 24) + player * (112 + gap);
        int y = HUD_Y + (gap ? 0 : 8);
        for (const char *c = amount; *c; ++c, x += 8) {
            for (int row = 0; row < 8; ++row) {
                unsigned bits = rcr_font_row(*c, row);
                for (int bit = 0; bit < 8; ++bit)
                    if (bits & (128u >> bit)) out[(y + row) * width + x + bit] = g_nes_palette[0x37];
            }
        }
    }
}

void rcr_wallet_frame(uint32_t *frame, int width) {
    wallet_players = 0;
    /* Select slides in the NES money HUD. Let it and the Start menu own
     * their native layouts throughout their transitions. */
    if (!street_scene() || g_ram[0x3f] || g_ram[0xde] || (g_ram[0x3e] & 2)) return;
    int origin = (width - 256) / 2;
    for (int row = 0; row < HUD_ROWS; ++row)
        memcpy(native_hud[row], frame + (HUD_Y + row) * width + origin, sizeof native_hud[row]);
    for (int player = 0; player < 2; ++player) {
        if (!(g_ram[0x48 + player] & 0x80)) continue;
        /* Wait for the actual name to appear after a fade or room load. */
        int name = 24 + player * 112;
        for (int row = 0; row < 8 && !(wallet_players & (1u << player)); ++row)
            for (int x = name; x < name + 40; ++x)
                if (native_hud[row][x] != native_hud[row][0]) {
                    wallet_players |= 1u << player;
                    break;
                }
    }
    if (width > 256) draw_wallet_hud(frame, width);
}

/* Fill newly revealed columns from the same guest frame. Retain the exact
 * displayed overlap (including native IRQ splits) and center the HUD. No PPU
 * render or guest IRQ is executed by the desktop menu. */
void game_pause_preview(uint32_t *out, int width, const uint32_t *frozen, int old_width) {
    if (width == old_width) {
        memcpy(out, frozen, (size_t)width * 240 * sizeof(*out));
        return;
    }
    int world = gameplay() && live_terrain.pages && !pause_panel();
    int old_view = old_width == 256 ? native_camera() : view_left(s_actors.present_camera, old_width);
    int new_view = width == 256 ? native_camera() : view_left(s_actors.present_camera, width);
    if (world) {
        for (int y = 32; y < 192; ++y) for (int x = 0; x < width; ++x) {
            int opaque;
            out[y*width+x] = bg_pixel(new_view+x, y, &opaque);
            s_opaque[y*width+x] = (uint8_t)opaque;
        }
        int previous_view = s_view;
        s_view = new_view;
        draw_blood(out,width,new_view);
        draw_actors(out, width);
        s_view = previous_view;
    }
    for (int y = 0; y < 240; ++y) {
        int playfield = world && y >= 32 && y < 192;
        int shift = playfield ? old_view-new_view : (width-old_width)/2;
        if (!playfield) {
            uint32_t fill = pause_panel() ? frozen[y*old_width+(old_width+256)/2-1] : 0xff000000;
            for (int x = 0; x < width; ++x) out[y*width+x] = fill;
        }
        int dst = shift > 0 ? shift : 0;
        int src = shift < 0 ? -shift : 0;
        int count = old_width-src < width-dst ? old_width-src : width-dst;
        if (count > 0)
            memcpy(out+y*width+dst, frozen+y*old_width+src, (size_t)count*sizeof(*out));
    }
    draw_wallet_hud(out, width);
}

/* Game lettering and rotated selection triangle from the owner's CHR bank 3. */
uint8_t rcr_font_row(char c, int row) {
    int tile;
    if (c >= 'A' && c <= 'Z') tile = c-'A';
    else if (c >= 'a' && c <= 'z') tile = 26+c-'a';
    else if (c >= '0' && c <= '9') tile = 52+c-'0';
    else if (c == '$') tile = 62;
    else if (c == '+') return row==3?0x7c:row>0&&row<6?0x10:0;
    else if (c == '-') return row==3?0x7c:0;
    else if (c == '.') return row==6 ? 0x18 : 0;
    else if (c == ',') return row==5?0x18:row==6?0x10:row==7?0x20:0;
    else if (c == '(' || c == ')') {
        static const uint8_t parens[8]={0x08,0x10,0x20,0x20,0x20,0x10,0x08,0};
        return c=='('?parens[row]:parens[row]==0x20?0x08:parens[row]==0x08?0x20:parens[row];
    }
    else if (c == ':') return row == 2 || row == 5 ? 0x18 : 0;
    else if (c == '?') {
        static const uint8_t question[8] = {0x38,0x44,0x04,0x18,0x10,0,0x10,0};
        return question[row];
    } else if (c == '>') {
        const uint8_t *glyph = s_rom + PRG_SIZE + 3*1024 + 63*16;
        uint8_t bits = 0;
        for (int x = 0; x < 8; ++x)
            if ((glyph[x] | glyph[8+x]) & (1u << row)) bits |= 128u >> x;
        return bits;
    } else return 0;
    const uint8_t *glyph = s_rom + PRG_SIZE + 3*1024 + tile*16;
    return glyph[row] | glyph[row+8];
}

void rcr_standing_preview(uint32_t *out, int stride, int x, int y, int player) {
    Packet pose={0};
    pose.source=(Source){.valid=1,.anim=player ? 0 : 0x80,.variant=player,.wy=237};
    decode_packet(&pose);
    /* Standing uses the first two gameplay sprite CHR pages (64 and 66).
     * Read them from ROM even when a guest submenu has banked them out. */
    static const uint8_t palette[2][4]={{0,0x0f,0x30,0x26},{0,0x0f,0x11,0x26}};
    for (int i=0;i<pose.count;++i) {
        Sprite *s=&pose.tiles[i];
        unsigned bank=s->tile<64 ? 64 : 66;
        const uint8_t *tile=s_rom+PRG_SIZE+bank*1024+(s->tile%64)*16;
        uint8_t tag=appearance_tag(s,player);
        for (int dy=0;dy<8;++dy) {
            int row=(s->attr&128)?7-dy:dy;
            uint8_t lo=tile[row], hi=tile[row+8];
            rcr_style_sprite_row(tag,s->tile,row,&lo,&hi);
            for (int dx=0;dx<8;++dx) {
                int bit=(s->attr&64)?dx:7-dx;
                int c=((lo>>bit)&1)|(((hi>>bit)&1)<<1);
                if (!c) continue;
                uint8_t color=rcr_sprite_color(tag,s->tile,row,bit,c,palette[s->attr&1]);
                out[(y+s->y+dy)*stride+x+s->x+dx]=g_nes_palette[color];
            }
        }
    }
}
