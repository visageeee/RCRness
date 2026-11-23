/* Experimental River City Ransom USA E9C387EC widescreen mod:
 * world-space presentation and visibility.
 * All graphics are read from the verified owner ROM at runtime. No ROM data
 * or generated assets are part of this source file. See docs/WIDESCREEN.md. */
#include "rcr_widescreen.h"
#include "nes_runtime.h"
#include "nes_video.h"
#include "mapper.h"
#include "config.h"
#include "mod_function_hooks.h"
#include "mod_savestate.h"
#include "debug_server.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROOMS 35
#define OBJECTS 7
#define TILES_PER_OBJECT 128
#define MAX_PAGES 5
#define PRG_SIZE 0x20000
#define CHR_SIZE 0x20000
typedef struct { int16_t x, y; uint8_t tile, attr, carried_owner, regions; uint16_t tile_source, attr_source; } Sprite;
typedef struct {
    int16_t x, wy;
    uint32_t order;
    uint8_t type, anim, variant, flags, held_by, held_state, held_kind, room, valid;
} Source;
typedef struct {
    Sprite tiles[TILES_PER_OBJECT];
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
static int s_room = -1, s_pages, s_left, s_right, s_view, s_camera;
static int s_resident;
static uint8_t s_tiles[MAX_PAGES * 32][30];
static uint8_t s_attributes[MAX_PAGES * 32][30];
static uint8_t s_page_palette[MAX_PAGES], s_page_chr[MAX_PAGES];
static uint8_t s_resident_nt[2048], s_resident_chr[2];
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
static int gameplay(void) {
    return s_ready && g_ram[0x42] < ROOMS && g_ram[0x41] == 0 &&
           g_ram[0x40] && (g_ram[0x48] & 0x80) && !(g_ram[0x3e] & 0x40) &&
           !(g_ram[0x36] & 0x40);
}
static int pause_panel(void) {
    /* The native full-submenu palettes use $0C in buffer $0110; the world
     * palettes restore $0F on return. Unlike
     * gameplay flags, this covers level/status/help as well as inventory
     * and passwords, and derives correctly from guest RAM on state load. */
    return (g_ram[0x3f]&0x20) && g_ram[0x110]==0x0c;
}
static uint8_t prg(int bank, unsigned addr) {
    if (addr >= 0xc000 && addr <= 0xffff) return s_rom[0x1c000 + addr - 0xc000];
    if (bank >= 0 && bank < 8 && addr >= 0x8000 && addr < 0xc000)
        return s_rom[bank * 0x4000 + addr - 0x8000];
    return 0;
}
static unsigned word(int bank, unsigned addr) { return prg(bank, addr) | (prg(bank, addr+1) << 8); }

static uint8_t screen_byte(unsigned bank, unsigned addr) {
    if (addr>=0x8000) return prg((int)bank,addr);
    unsigned offset=PRG_SIZE+bank*1024+addr-0x1000;
    return addr>=0x1000 && addr<0x2000 && offset<sizeof s_rom ? s_rom[offset] : 0;
}
/* C7F2..C928: bounded resident-screen bytecode, including substreams. */
static int screen_stream(unsigned bank, unsigned *src, unsigned *dst, int depth, int *budget) {
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
                if (!screen_stream(bank,&sub,dst,depth+1,budget)) return 0;
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
                if (*dst>=0x2000 && *dst<0x2800) s_resident_nt[*dst-0x2000]=(uint8_t)value;
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
static void decode_room(void) {
    s_room = g_ram[0x42]; s_pages = 0; s_left = 0; s_right = 256; s_resident=0;
    memset(s_tiles, 0, sizeof s_tiles);
    memset(s_attributes, 0, sizeof s_attributes);
    unsigned directory = 0xc599 + s_room * 3;
    if (!(prg(7, directory+2) & 0x80)) {
        /* C000..C091 loads the resident C959 screen records into the two
         * horizontal nametables. Directory allocation distinguishes paired
         * scenes from single-screen scenes sharing the next record. */
        unsigned first=prg(7,directory+2), end=0x26;
        for (int room=0; room<ROOMS; room++) {
            unsigned next=prg(7,0xc59b+room*3);
            if (next>first && next<end) end=next;
        }
        if (end-first==2) {
            memset(s_resident_nt,0,sizeof s_resident_nt);
            for (int p=1;p>=0;p--) {
                unsigned desc=0xc959+(first+p)*6;
                unsigned src=word(7,desc), dst=0x2000;
                int budget=16384;
                if (!screen_stream(prg(7,desc+2),&src,&dst,0,&budget)) {
                    s_decode_errors++; return;
                }
            }
            unsigned desc=0xc959+first*6;
            s_resident_chr[0]=prg(7,desc+4)&0xfe;
            s_resident_chr[1]=prg(7,desc+5)&0xfe;
            s_resident=1; s_pages=2; s_right=512;
        }
        return;
    }
    unsigned first = word(7, directory), end = 0xc705;
    for (int room=0; room<ROOMS; room++) {
        unsigned p = word(7, 0xc599+room*3);
        if (p > first && p < end) end=p;
    }
    if (first < 0xc602 || end <= first || (end-first)%7) { s_decode_errors++; return; }
    s_pages = (int)((end-first)/7);
    if (s_pages > MAX_PAGES) { s_pages=0; s_decode_errors++; return; }
    /* The final corridor has four authored pages and a duplicate fifth page
     * for the native column stream's lookahead. The original right wall stops
     * Alex at X=1015. Cache capacity must not become another visible room. */
    if (s_room==0x20 && s_pages==5 &&
        !memcmp(s_rom+0x1c000+first-0xc000+21,
                s_rom+0x1c000+first-0xc000+28,7)) s_pages=4;
    for (int p=0; p<s_pages; p++) {
        unsigned desc = first+p*7;
        unsigned rows = word(7,desc), attrs = word(7,desc+2);
        int bank = prg(7,desc+6)&3;
        s_page_palette[p] = prg(7,desc+4);
        s_page_chr[p] = prg(7,desc+5);
        for (int y=4; y<24; y++) {
            unsigned row = word(bank,rows+y*2);
            if (row < 0x8000 || row > 0xbfe0 || attrs < 0x8000 || attrs > 0xbfc0) {
                s_pages=0; s_decode_errors++; return;
            }
            for (int x=0; x<32; x++) {
                s_tiles[p*32+x][y] = prg(bank,row+x);
                uint8_t a = prg(bank,attrs+(y/4)*8+x/4);
                s_attributes[p*32+x][y] = (a >> (((y&2)*2)+(x&2))) & 3;
            }
        }
    }
    s_right = s_pages*256;
}
static int view_left(int cam, int width) {
    int left = cam-(width-256)/2;
    if (s_right-s_left <= width) return s_left-(width-(s_right-s_left))/2;
    if (left < s_left) left=s_left;
    if (left > s_right-width) left=s_right-width;
    return left;
}
static uint32_t bg_pixel(int wx, int y, int *opaque) {
    *opaque=0;
    if (wx < s_left || wx >= s_right || y<32 || y>=192) return 0xff000000u;
    if (s_resident) {
        unsigned nt=(wx/256)*1024, tx=(wx&255)/8, ty=y/8;
        unsigned tile=s_resident_nt[nt+ty*32+tx];
        unsigned attr=s_resident_nt[nt+960+(ty/4)*8+tx/4];
        unsigned pal=(attr>>((ty&2)*2+(tx&2)))&3;
        unsigned row=PRG_SIZE+s_resident_chr[tile/128]*1024+(tile%128)*16+(y&7), shift=7-(wx&7);
        if (row+8>=sizeof s_rom) return 0xff000000u;
        unsigned pixel=((s_rom[row]>>shift)&1)|(((s_rom[row+8]>>shift)&1)<<1);
        *opaque=pixel!=0;
        return g_nes_palette[g_ppu_pal[pixel?pal*4+pixel:0]&63];
    }
    int p=wx/256, tile=s_tiles[wx/8][y/8], pal=s_attributes[wx/8][y/8];
    unsigned offset = PRG_SIZE+(s_page_chr[p]&0xfe)*1024+tile*16+(y&7);
    if (offset+8 >= sizeof s_rom) return 0xff000000u;
    int shift=7-(wx&7);
    int pixel=((s_rom[offset]>>shift)&1)|(((s_rom[offset+8]>>shift)&1)<<1);
    *opaque=pixel!=0;
    int color;
    if (s_page_palette[p] == g_ram[0x518]) color=g_ppu_pal[pixel ? pal*4+pixel : 0];
    else {
        unsigned palette=word(7,0xe6f1+s_page_palette[p]*2);
        color=prg(7,palette+(pixel ? pal*4+pixel : 0));
        if ((g_ram[0x3e]&2) && g_ram[0x692]<4 && color!=0x0f) {
            int brightness=g_ram[0x692]*16;
            if ((color&0x30)>=brightness) color=(color&15)|brightness;
        }
    }
    return g_nes_palette[color&63];
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
 * two bits (hair). The same tags travel with native and repaired OAM. */
enum { SHIRT = 1, HAIR = 4 };
static uint8_t appearance_tag(const Sprite *s, int obj) {
    int owner=s->carried_owner ? s->carried_owner : obj+1;
    return owner<=2 ? owner*s->regions : 0;
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
uint8_t rcr_sprite_color(uint8_t tag, uint8_t tile, int row, int bit, int pixel, uint8_t original) {
    if (pixel==2) return rcr_shirt_color(tag & 3, original);
    int owner=tag >> 2;
    if (pixel==1 && owner>=1 && owner<=2 && original==0x0f && (hair_region[tile][row] & (1u<<bit))) {
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
        if (s->regions && tile==s->tile_source && (s->attr_source ? attr==s->attr_source : !g_ram[0x31])) {
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

/* Decode ED0E..EF88's metasprite bytecode, including its linked streams,
 * per-character palettes, held objects and alternate face tiles. Signed world
 * coordinates replace only the final 8-bit screen clip. Guest RAM, OAM and
 * timing are untouched. Packets are promoted at the same DMA as native OAM. */
static void decode_packet(Packet *out) {
    const Source *src=&out->source;
    out->count=0; out->room=src->room; out->order=src->order;
    if (!src->valid) return;
    int type=src->type, anim=src->anim;
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
        if (!count) return;
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
                unsigned kind=src->held_kind;
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
            s->x=(int16_t)(src->x+(flip ? -dx-1 : dx)-4);
            s->y=(int16_t)(237-wy-dy);
            /* $FD swaps subpalettes 0 -> 1 on the shirt, and 1 -> 0 on
             * trousers. Face and held-item streams keep their own attributes. */
            if (carried_owner) {
                alex_attr=carried_alex_attr;
                ryan_attr=carried_ryan_attr;
            }
            s->carried_owner=(uint8_t)carried_owner;
            int shirt = (carried_owner || type == 0) && alex_attr && (prg(3,alex_attr+n)&3)==0 &&
                       (prg(3,ryan_attr+n)&3)==1;
            static const uint8_t empty_region[8]={0};
            int hair=(carried_owner || (!attachment && type==0)) &&
                     memcmp(hair_region[tile],empty_region,sizeof empty_region);
            s->regions=(shirt ? SHIRT : 0) | (hair ? HAIR : 0);
            s->tile_source=tiles+n;
            s->attr_source=attr_ptr ? attr_ptr+n : 0;
            s->tile=(uint8_t)tile;
            s->attr=(uint8_t)((attr_ptr ? prg(3,attr_ptr+n) : attr)^(flip?0x40:0));
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
    return s_enabled && s_pages>0 && s_room==g_ram[0x42] &&
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
static void draw_packet(uint32_t *out, int width, const Packet *packet, int owner) {
    if (packet->room != g_ram[0x42]) return;
    for (int n=0; n<packet->count; n++) {
        const Sprite *s=&packet->tiles[n];
        uint8_t tag=appearance_tag(s,owner-1);
        for (int y=0; y<8; y++) {
            int py=s->y+y;
            if (py<32 || py>=192) continue;
            int row=(s->attr&0x80)?7-y:y;
            int offset=0x1000+s->tile*16+row;
            for (int x=0; x<8; x++) {
                int px=s->x-s_view+x;
                if (px<0 || px>=width) continue;
                int bit=(s->attr&0x40)?x:7-x;
                int c=((g_chr_ram[offset]>>bit)&1)|(((g_chr_ram[offset+8]>>bit)&1)<<1);
                if (!c || ((s->attr&0x20)&&s_opaque[py*width+px])) continue;
                uint8_t color=g_ppu_pal[0x10+(s->attr&3)*4+c]&63;
                if (tag) color=rcr_sprite_color(tag,s->tile,row,bit,c,color);
                out[py*width+px]=g_nes_palette[color];
            }
        }
    }
}
static void draw_actors(uint32_t *out, int width) {
    int order[OBJECTS];
    for (int i=0; i<OBJECTS; i++) order[i]=i;
    for (int i=0; i<OBJECTS; i++) for (int j=i+1; j<OBJECTS; j++)
        if (s_actors.present[order[j]].order<s_actors.present[order[i]].order) {
            int t=order[i]; order[i]=order[j]; order[j]=t;
        }
    for (int i=0; i<OBJECTS; i++) draw_packet(out,width,&s_actors.present[order[i]],order[i]+1);
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
    if (!s_pages) return 0;
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
    if (!s_pages) return value;
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
    if (strcmp(key,"--widescreen") || !value) return 0;
    NesAspectMode mode;
    if (!nes_video_aspect_from_name(value,&mode)) return 0;
    s_enabled=mode!=NES_ASPECT_STOCK;
    if (s_enabled) s_aspect=mode;
    apply(); return 1;
}
void rcr_ws_frame(void) {
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
} ActorSave;
static int state_get(uint8_t *buf, int cap) {
    if (cap<(int)sizeof(ActorSave)) return -1;
    ActorSave state={0};
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
    if (len!=sizeof(ActorSave) && len!=offsetof(ActorSave,shadow_camera) && len!=offsetof(ActorSave,list_pending)) return 0;
    ActorSave state={0}; memcpy(&state,buf,(size_t)len);
    if (state.list_pending>1) return 0;
    if (state.room>=ROOMS) return 0;
    for (int i=0;i<OBJECTS;i++) {
        if (state.shadow[i].valid>1 || state.present[i].valid>1 ||
            state.shadow[i].type>=0x40 || state.present[i].type>=0x40) return 0;
    }
    memset(&s_actors,0,sizeof s_actors);
    s_actors.sequence=state.sequence; s_actors.room=(uint8_t)state.room;
    s_list_pending=state.list_pending;
    s_actors.shadow_camera=len==sizeof state ? state.shadow_camera : camera();
    s_actors.present_camera=len==sizeof state ? state.present_camera : camera();
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
    buf[192]=1;
    memcpy(buf,shadow_tags,64); memcpy(buf+64,display_tags,64); memcpy(buf+128,coherent_tags,64);
    return 193;
}
static int shirt_state_set(const uint8_t *buf, int size) {
    if (size==192) {
        /* Older saves only owned shirt ink. Retain the reconstructed hair tags. */
        for (int i=0;i<64;i++) {
            shadow_tags[i]=(shadow_tags[i]&12)|(buf[i]&3);
            display_tags[i]=(display_tags[i]&12)|(buf[64+i]&3);
            coherent_tags[i]=(coherent_tags[i]&12)|(buf[128+i]&3);
        }
        return 1;
    }
    if (size!=193 || buf[192]!=1) return 0;
    memcpy(shadow_tags,buf,64); memcpy(display_tags,buf+64,64); memcpy(coherent_tags,buf+128,64);
    return 1;
}
void rcr_ws_init(const char *rom_path) {
    FILE *f=rom_path?fopen(rom_path,"rb"):NULL;
    if (!f) { fprintf(stderr,"[Widescreen] Cannot read owner ROM\n"); return; }
    int ok=fseek(f,16,SEEK_SET)==0 && fread(s_rom,1,sizeof s_rom,f)==sizeof s_rom;
    fclose(f);
    if (!ok) { fprintf(stderr,"[Widescreen] Incomplete owner ROM\n"); return; }
    s_ready=1;
    nes_video_on_window_resized(1280,720);
    nes_mod_register_function_entry_plugin("rcr.widescreen.metasprite",0xed0e,observe_sprite);
    /* Native-width play also supplies actors for the frozen wide preview. */
    nes_mod_set_function_hook_enabled("rcr.widescreen.metasprite",1);
    nes_mod_register_savestate_hook("rcr.widescreen.actors",state_get,state_set);
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
    if (strcmp(cmd,"rcr_widescreen")) return 0;
    debug_server_send_fmt("{\"id\":%d,\"ok\":true,\"width\":%d,\"room\":%d,\"pages\":%d,\"camera\":%d,\"view\":%d,\"wide_frames\":%llu,\"decode_errors\":%llu,\"collision_reads\":%llu,\"activation_reads\":%llu,\"spawn_reads\":%llu,\"sprites\":[%u,%u,%u,%u,%u,%u,%u],\"terrain_pixels\":%u,\"terrain_diffs\":%u,\"pause_panel\":%u}",
        id,g_render_width,s_room,s_pages,s_camera,s_view,(unsigned long long)s_wide_frames,
        (unsigned long long)s_decode_errors,(unsigned long long)s_collision_reads,
        (unsigned long long)s_activation_reads,(unsigned long long)s_spawn_reads,
        s_actors.present[0].count,s_actors.present[1].count,s_actors.present[2].count,
        s_actors.present[3].count,s_actors.present[4].count,s_actors.present[5].count,s_actors.present[6].count,
        s_terrain_pixels,s_terrain_diffs,(unsigned)pause_panel());
    return 1;
}

/* Fill newly revealed columns from the same guest frame. Retain the exact
 * displayed overlap (including native IRQ splits) and center the HUD. No PPU
 * render or guest IRQ is executed by the desktop menu. */
void game_pause_preview(uint32_t *out, int width, const uint32_t *frozen, int old_width) {
    if (width == old_width) {
        memcpy(out, frozen, (size_t)width * 240 * sizeof(*out));
        return;
    }
    int world = gameplay() && s_pages && !pause_panel();
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
}

/* Game lettering and rotated selection triangle from the owner's CHR bank 3. */
uint8_t rcr_font_row(char c, int row) {
    int tile;
    if (c >= 'A' && c <= 'Z') tile = c-'A';
    else if (c >= 'a' && c <= 'z') tile = 26+c-'a';
    else if (c >= '0' && c <= '9') tile = 52+c-'0';
    else if (c == '+') return row==3?0x7c:row>0&&row<6?0x10:0;
    else if (c == '-') return row==3?0x7c:0;
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
        for (int dy=0;dy<8;++dy) for(int dx=0;dx<8;++dx) {
            int row=(s->attr&128)?7-dy:dy, bit=(s->attr&64)?dx:7-dx;
            int c=((tile[row]>>bit)&1)|(((tile[row+8]>>bit)&1)<<1);
            if (!c) continue;
            uint8_t color=palette[s->attr&1][c];
            color=rcr_sprite_color(appearance_tag(s,player),s->tile,row,bit,c,color);
            out[(y+s->y+dy)*stride+x+s->x+dx]=g_nes_palette[color];
        }
    }
}
