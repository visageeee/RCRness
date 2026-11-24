/* Original synthetic white-box test: no ROM or generated game code. */
#define SDL_MAIN_HANDLED
#include "../rcr_widescreen.c"
#include <assert.h>
NesConfig g_nes_config;
uint8_t g_ppu_oam[256];
uint8_t g_ram[0x800];
uint8_t g_chr_ram[0x2000], g_ppu_pal[0x20];
const uint32_t g_nes_palette[64]={0};
CPU6502State g_cpu;
int g_current_bank, g_render_width=426;
/* Unused host integrations are stubbed; the sprite/list/state functions
 * exercised below are the real implementation included above. */
int nes_video_aspect_from_name(const char *n,NesAspectMode *m) { (void)n;(void)m;return 0; }
void nes_video_set_aspect_mode(NesAspectMode m) { (void)m; }
void nes_video_on_window_resized(int w,int h) { (void)w;(void)h; }
const char *nes_video_aspect_name(NesAspectMode m) { (void)m;return "test"; }
int ppu_renderer_set_custom_render(NesCustomRenderFn f,void *u) { (void)f;(void)u;return 1; }
void ppu_renderer_set_sprite_suppress(PpuSpriteSuppressFn f,void *u) { (void)f;(void)u; }
int ppu_renderer_in_custom_native_pass(void) { return 0; }
int ppu_renderer_background_opaque(int x,int y) { (void)x;(void)y;return 0; }
int nes_mod_set_function_hook_enabled(const char *id,int e) { (void)id;(void)e;return 1; }
int nes_mod_register_function_entry_plugin(const char *id,uint16_t a,NESModFunctionEntryCallback f) {
    (void)id;(void)a;(void)f;return 1;
}
int nes_mod_register_savestate_hook(const char *id,NESModSavestateGet g,NESModSavestateSet s) {
    (void)id;(void)g;(void)s;return 1;
}
void debug_server_send_fmt(const char *format,...) { (void)format; }

static void test_blood(void) {
    static uint32_t frame[256*240];
    g_nes_config.blood_effects=1;
    /* Two co-op victims at different lane depths, one in the air. */
    for (int player=0;player<2;++player) {
        g_ram[0x83+player]=60+80*player;
        g_ram[0x9e + player]=90+24*player;
        g_ram[0xb9+player]=12*player;
        rcr_blood_hit(player,0);
    }
    for (int tick=0;tick<48;++tick) blood_step();
    assert(blood_drop_next==2*HIT_SPRAY);
    g_ram[0x42]=1;
    blood_step();
    draw_blood(frame,256,0);
    for (int i=0;i<256*240;i++) assert(!frame[i]);
    g_ram[0x42]=0;
    blood_step();
    draw_blood(frame,256,30);
    int pixels=0;
    for (int i=0;i<256*240;i++) if (frame[i]) ++pixels;
    assert(pixels>30);
    /* A full journal survives a restart, a partial tail, and another hit. */
    blood_file=tmpfile();
    assert(blood_file);
    strcpy(blood_path,"test");
    for (int x=0;x<512;++x) save_stain(3,x,140);
    fputc(131,blood_file); fputc(0,blood_file);
    save_stain(4,900,180);
    rewind(blood_file);
    memset(blood_stains,0,sizeof blood_stains);
    load_stains(blood_file);
    for (int x=0;x<512;++x) assert(!stain_pixel(3,x,140));
    assert(!stain_pixel(4,900,180));
    rcr_new_game();
    assert(!blood_file && !blood_pending && blood_game==UINT32_MAX);
    assert(!room_stained[3] && !room_stained[4]);
    assert(stain_pixel(3,0,140) && stain_pixel(4,900,180));
    /* Count each burst at the damage callback, including a held weapon
     * during a kick, a weapon swing by an enemy, and a thrown object. */
    static const struct { int source,move,drops; } hits[]={
        {0,0x1c,HIT_SPRAY}, {1,0xa0,WEAPON_SPRAY}, {2,0x21,WEAPON_SPRAY}, {3,0x2a,WEAPON_SPRAY},
        {0,0xa3,WEAPON_SPRAY}, {4,0x16,WEAPON_SPRAY}, {2,0x16,HIT_SPRAY}, {255,0,HIT_SPRAY}
    };
    for (unsigned i=0;i<sizeof hits/sizeof hits[0];i++) {
        g_ram[5]=hits[i].source;
        if (hits[i].source<OBJECTS) {
            g_ram[0x4f+hits[i].source]=hits[i].move;
            g_ram[0x56+hits[i].source]=0x40;
            g_ram[0x3b7+hits[i].source]=4;
        }
        rcr_blood_hit(0,0);
        assert(blood_drop_next==hits[i].drops);
        for (int tick=0;tick<64;tick++) blood_step();
        for (int j=0;j<BLOOD_DROPS;++j) assert(!blood_drops[j].floor);
        rcr_clear_blood_spray();
    }
    g_ram[5]=0;
    g_ram[0x4f]=0x20;
    g_ram[0x6a]=1;
    rcr_blood_hit(2,1);
    assert(!head_items && blood_drop_next==WEAPON_SPRAY); /* missed roll */
    g_ram[0x6a]=0;
    g_ram[0x4c]=g_ram[0x4d]=g_ram[0x4e]=0x86;
    rcr_blood_hit(2,1);
    assert(!head_items && street_items[0].kind && headless_fighters==(1<<2));
    for (int tick=0;tick<64;++tick) street_step();
    assert(street_items[0].height==0 && g_ram[0x4c]==0x86);
    rcr_object_replaced(2);
    /* A second successful finisher still drops with all guest slots busy. */
    assert(drop_head(2,1) && street_items[1].kind);
    rcr_object_replaced(2);
    /* Full pool from a street fight: a loose weapon on either side of
     * Alex, with the middle slot held. Protect Ryan's nearby pickup too. */
    for (int slot=4;slot<7;slot++) {
        g_ram[0xcb+slot]=15;
        g_ram[0x4f+slot]=0x80;
        g_ram[0x56+slot]=0;
        g_ram[0x9e + slot]=90;
    }
    g_ram[0x83+4]=85; g_ram[0x83+5]=29; g_ram[0x83+6]=210;
    g_ram[0x4f+5]=0x82; g_ram[0x56+5]=0x40;
    assert(ground_slot(g_ram[5])==6);
    g_ram[0x49]=0x81; g_ram[0x84]=210; g_ram[0x9f]=90;
    assert(ground_slot(g_ram[5])==4);
    g_ram[0x49]=0;
    g_ram[0x4f+6]=0x84;
    assert(ground_slot(g_ram[5])==4);
    g_ram[0x4f+4]=0x86;
    assert(ground_slot(g_ram[5])==-1);
    /* Move close to the stored head: the far loose weapon trades places
     * with it, retaining its pickup position and appearance. */
    g_ram[0x4f+6]=0x80;
    street_step();
    assert((head_items&(1<<6)) && street_items[0].type==15 && street_items[0].x==210);
    uint8_t snapshot[512];
    int size=state_get(snapshot,sizeof snapshot);
    enter_street(1);
    assert(!street_items[1].type && state_set(snapshot,size));
    assert(state_set(snapshot,offsetof(ActorSave,blood_game)) && blood_game==0);
    assert(street_items[1].kind && street_items[0].type==15);
    uint8_t guest_snapshot[sizeof g_ram];
    memcpy(guest_snapshot,g_ram,sizeof guest_snapshot);
    for (int i=2;i<STREET_SLOTS;++i) {
        rcr_object_replaced(2);
        assert(drop_head(2,1));
        assert(street_items[i].type);
    }
    rcr_object_replaced(2);
    assert(!drop_head(2,1));
    memcpy(g_ram,guest_snapshot,sizeof guest_snapshot);
    assert(state_set(snapshot,size));
    g_ram[0x83]=210;
    street_step();
    assert(g_ram[0xcb+6]==15 && !(head_items&(1<<6)) && street_items[0].kind);
    assert(state_set(snapshot,offsetof(ActorSave,street)) && !street_items[0].type);
    rcr_object_replaced(6);
    g_ram[0x4d]=0;
    rcr_blood_hit(1,1);
    assert(!head_items); /* co-op player */
    rcr_clear_blood_spray();
    g_ram[0x4f]=0x1a;
    rcr_blood_hit(2,1);
    assert(!head_items && blood_drop_next==HIT_SPRAY); /* unarmed */
    g_ram[0x4f]=0x20;
    rcr_blood_hit(2,0);
    assert(!head_items); /* nonlethal */
    rcr_clear_blood_spray();
    rcr_blood_hit(2,1);
    assert(head_items==(1<<5) && headless_fighters==(1<<2) && blood_drop_next==FINISHER_SPRAY);
    assert(g_ram[0xcb+5]==14 && g_ram[0x4f+5]==4);
    rcr_object_replaced(2);
    assert(!headless_fighters && head_items==(1<<5));
    rcr_object_replaced(5);
    assert(!head_items);
    g_ram[0x4c]=g_ram[0x4d]=g_ram[0x4e]=0;
    rcr_clear_blood_spray();
    g_nes_config.blood_effects=0;
    rcr_blood_hit(0,0);
    assert(blood_room==-1);
    uint8_t lo=0x39, hi=0x56;
    rcr_style_sprite_row(HIDDEN,0,4,&lo,&hi);
    assert(lo==0x39 && hi==0x56); /* Turning gore off restores guest art. */
}

static void test_chainsaw(void) {
    uint8_t original_ram[sizeof g_ram], saved[NES_MOD_SAVESTATE_BLOB_CAP];
    memcpy(original_ram,g_ram,sizeof g_ram);
    assert(spawn_saw(0));
    street_step();
    assert(has_saw(4));
    g_ram[0x56]=0x40;
    g_ram[0x3b7]=4;
    /* Only the metasprite lookups for this carrier change. Both facings
     * retain their mirror bit through carry, windup and extension. */
    for (int facing=0;facing<=128;facing+=128) {
        assert(rcr_saw_graphics_read(0xed3b,0xd3,facing|4)==facing);
        assert(saw_pose(facing|8)==facing);
        assert(saw_pose(facing|9)==(facing|7));
        assert(rcr_saw_graphics_read(0xed3b,0xd3,facing|0x2c)==(facing|1));
        assert(rcr_saw_graphics_read(0xed3b,0xd3,facing|0x2d)==(facing|0x2a));
        assert(rcr_saw_graphics_read(0xed3b,0xd3,facing|0x7f)==(facing|0x7f));
    }
    /* Follow one bright tooth along each edge over a complete revolution
     * of the four-pixel pattern. The housing moves by one pixel only. */
    for (int phase=0;phase<4;++phase) {
        g_ram[0x52b]=phase;
        int shake=phase&1;
        assert(saw_ink(HELD_SAW_ART+1,2+shake,7-phase)=='s');
        assert(saw_ink(HELD_SAW_ART+1,6+shake,7-((4-phase)&3))=='s');
        assert(saw_ink(HELD_SAW_ART,shake,6)=='#');
        assert(saw_ink(SAW_ART,0,6)=='#');
        if (shake) assert(saw_ink(HELD_SAW_ART,0,6)=='.');
    }
    assert(rcr_saw_graphics_read(0xee4f,0x3b0,6)==HAND_WEAPON_KIND);
    assert(rcr_saw_graphics_read(0xee4f,0x3b1,6)==6);
    g_ram[0x4f]=0xa3;
    g_ram[0x85]=22;
    g_nes_config.blood_effects=g_nes_config.persistent_bodies=1;
    rcr_blood_hit(2,0);
    assert(blood_drop_next==SAW_SPRAY);
    rcr_clear_blood_spray();
    rcr_enemy_coin(2);
    g_ram[0x6f]=128;
    cut_bodies();
    assert(!bodies.body[0].cut);
    g_ram[0x6f]=0;
    cut_bodies();
    cut_bodies();
    assert(bodies.body[0].cut && blood_drop_next==SAW_SPRAY);
    int size=bodies_get(saved,sizeof saved);
    assert(bodies_set(saved,size) && bodies.body[0].cut);
    assert(bodies_set(saved,sizeof bodies) && !bodies.body[0].cut);
    rcr_clear_blood_spray();
    rcr_object_replaced(4);
    assert(!has_saw(4));
    bodies.next=bodies.count=0;
    g_nes_config.blood_effects=g_nes_config.persistent_bodies=0;
    memcpy(g_ram,original_ram,sizeof g_ram);
}

static void test_persistent_bodies(void) {
    uint8_t saved[NES_MOD_SAVESTATE_BLOB_CAP];
    g_nes_config.persistent_bodies=0;
    rcr_enemy_coin(2);
    assert(!bodies.count);
    g_nes_config.persistent_bodies=1;
    for (int i=0;i<BODY_LIMIT+1;++i) {
        g_ram[0x42]=i%2;
        g_ram[0x85]=i;
        rcr_enemy_coin(2);
    }
    assert(bodies.count==BODY_LIMIT && bodies.next==1);
    assert(bodies.body[0].x==BODY_LIMIT && bodies.body[1].room==1);
    int size=bodies_get(saved,sizeof saved);
    bodies.count=bodies.next=0;
    assert(size>0 && bodies_set(saved,size));
    assert(bodies.count==BODY_LIMIT && body_packets[1].source.room==1);
    bodies.count=bodies.next=0;
    g_nes_config.persistent_bodies=0;
    g_ram[0x85]=0;
}

int main(void) {
    uint8_t saved[512];
    s_ready=1; s_enabled=1;
    g_ram[0x40]=1; g_ram[0x48]=0x80; g_ram[0x4b]=0x80;
    test_chainsaw();
    test_persistent_bodies();
    test_blood();
    s_actors.room=0;
    s_actors.shadow[3].count=1;
    s_actors.shadow[3].source.valid=1;
    s_actors.present[3]=s_actors.shadow[3];

    /* $EFD4 can wait on DMA before the guest reads the new count. */
    g_ram[0x6b5]=2;
    rcr_ws_dma();
    assert(s_actors.present[3].count==1);
    assert(rcr_ws_read(0xefd9,0x6b5,2)==2);
    assert(s_list_pending);
    rcr_ws_dma();
    assert(s_actors.present[3].count==1);

    /* Two one-tile actors, one beyond the native viewport. At the first
     * visit the guest has decremented $06B5 and selected $069D[count]. */
    s_rom[0xc001]=0x81; /* animation table at $8100 */
    s_rom[0xc101]=0x82; /* stream at $8200 */
    s_rom[0xc200]=1;
    s_rom[0xc203]=0x83; /* tile and offset arrays at $8300 and $8400 */
    s_rom[0xc205]=0x84;
    g_ram[0x49]=0x81;
    g_ram[0x8d]=1; g_ram[0x84]=32;
    g_ram[0x69d]=1;
    g_ram[0x69e]=0;
    g_ram[0x6b5]=1;
    g_ram[0x47]=0;
    g_ram[0xdc]=120;
    observe_sprite(0xed0e);
    g_ram[0xdc]=122;
    assert(!s_list_pending && !s_actors.shadow[3].source.valid);
    assert(s_actors.present[3].count==1);
    rcr_ws_dma();
    assert(!s_actors.present[3].count);
    assert(s_actors.present[0].count==1 && s_actors.present[1].count==1);
    assert(s_actors.present[1].tiles[0].x==284);
    uint32_t second=s_actors.present[1].order;
    assert(second==s_actors.present[0].order+1);
    g_ram[0x6b5]=0;
    g_ram[0x47]=1;
    observe_sprite(0xed0e);
    rcr_ws_dma();
    assert(s_actors.present[1].order==second);
    assert(s_actors.present_camera==120);
    g_ram[0xdc]=0; g_ram[0xdd]=1; g_ram[0xf6]=254;
    assert(native_camera()==254);

    /* A blinking actor stays hidden and an omitted active actor retires. */
    rcr_ws_read(0xefd9,0x6b5,1);
    rcr_ws_dma(); /* Count already decremented for the sole upcoming visit. */
    assert(s_list_pending && s_actors.present[0].count==1);
    g_ram[0xd4]=0x7f;
    observe_sprite(0xed0e);
    rcr_ws_dma();
    assert(!s_actors.present[1].count && !s_actors.present[0].source.valid);

    /* An empty list makes no ED0E calls. */
    rcr_ws_read(0xefd9,0x6b5,0);
    rcr_ws_dma();
    assert(!s_list_pending && !s_actors.present[1].source.valid);

    /* A suspension before the first actor preserves the list phase. */
    s_list_pending=1;
    int size=state_get(saved,sizeof saved);
    assert(size>0);
    s_list_pending=0;
    s_actors.present_camera=0;
    assert(state_set(saved,size) && s_list_pending==1);
    assert(s_actors.present_camera==s_actors.shadow_camera);
    assert(state_set(saved,offsetof(ActorSave,shadow_camera)) && s_list_pending==1);
    assert(s_actors.present_camera==camera());
    /* Old v0.1.6 payloads still load without the appended phase field. */
    assert(state_set(saved,offsetof(ActorSave,list_pending)) && !s_list_pending);
    /* The carried fighter belongs to port 2 even in enemy slot 3. The
     * attribute table overlaps a face stream with a different tile source. */
    Packet *carrier=&s_actors.shadow[3];
    carrier->count=1;
    carrier->tiles[0]=(Sprite){.regions=SHIRT,.tile_source=0x917f, .attr_source=0x8e24, .carried_owner=2};
    g_ram[0x47]=3;
    g_cpu.Y=0;
    g_ram[0x2c]=0x7f; g_ram[0x2d]=0x91;
    g_ram[0x30]=0x24; g_ram[0x31]=0x8e;
    g_nes_config.reduce_sprite_flicker=1;
    rcr_oam_sprite_write(0xeeaa,3);
    rcr_appearance_dma(2,g_ppu_oam,0);
    assert(display_tags[3]==2);
    ++g_ram[0x2c];
    rcr_oam_sprite_write(0xeeeb,3);
    assert(!shadow_tags[3]);
    g_ppu_oam[12]=0xf8;
    rcr_appearance_dma(2,g_ppu_oam,1);
    assert(display_tags[3]==2);
    g_ppu_oam[12]=120;
    rcr_appearance_dma(2,g_ppu_oam,1);
    assert(!display_tags[3]);
    g_nes_config.outfit[1]=2;
    assert(rcr_shirt_color(2,0x11)==0x28);
    assert(rcr_shirt_color(2,0x30)==0x30);
    assert(rcr_shirt_color(1,0x30)==0x30);
    const uint8_t normal[]={0,0x0f,0x30,0x26}, blue[]={0,0x0f,0x11,0x26};
    const uint8_t flash[]={0,0x30,0x16,0x26}, dark[]={0,0x0f,0x0f,0x0f};
    g_nes_config.hair[0]=3;
    assert(rcr_sprite_color(HAIR,0,0,6,1,normal)==0x1a);
    assert(rcr_sprite_color(HAIR,0,7,6,1,normal)==0x0f);
    assert(rcr_sprite_color(HAIR,0,0,6,3,normal)==0x26);
    assert(rcr_sprite_color(HAIR,0,0,6,1,flash)==0x30);
    assert(rcr_sprite_color(0,0,0,6,1,normal)==0x0f);
    g_nes_config.pants[0]=3;
    g_nes_config.pants[1]=5;
    assert(rcr_sprite_color(PANTS,0x30,0,3,2,blue)==0x1a);
    assert(rcr_sprite_color(2*PANTS,0x30,0,3,2,normal)==0x14);
    assert(rcr_sprite_color(PANTS,0x30,0,3,2,flash)==0x16);
    assert(rcr_sprite_color(0,0x30,0,3,2,blue)==0x11);
    for (int panel=1;panel<=ONI_FALL_LOWER;panel++)
        for (int row=0;row<8;row++) assert(strlen(oni_panels[panel][row])==16);
    for (int tile=0;tile<256;tile++) {
        if (!oni_motion_tiles[tile]) continue;
        assert(strlen(oni_motion_tiles[tile])==64);
        assert(!oni_tiles[tile]); /* Every movement tile has one source of art. */
    }
    const uint8_t stride_and_kick[]={4,5,6,0x18,0x19,0x1a,0x28,0x29,0x2a,0x39,0x3a};
    for (unsigned i=0;i<sizeof stride_and_kick;i++) assert(oni_has_tile(stride_and_kick[i]));
    assert(!oni_has_tile(0x5e) && !oni_has_tile(0xe5));
    g_nes_config.oni_mode=1;
    for (int obj=0;obj<3;obj++) {
        Sprite head={.regions=BODY|HAIR};
        uint8_t tag=appearance_tag(&head,obj), lo=255, hi=0;
        rcr_style_sprite_row(tag,0,0,&lo,&hi);
        assert(lo==(obj<2 ? 8 : 255) && hi==0);
        assert(rcr_sprite_color(tag,0,0,3,1,normal)==(obj<2 ? 0x24 : 0x0f));
        assert(rcr_sprite_color(tag,0,0,3,1,dark)==0x0f);
    }
    /* Restore a v2 repaired slot without a visible OAM entry to match. */
    memset(saved,0,193);
    saved[128+3]=2*PANTS;
    saved[192]=2;
    assert(shirt_state_set(saved,193));
    assert(coherent_tags[3]==2*(PANTS|BODY));
    assert(shirt_state_get(saved,sizeof saved)==193 && saved[192]==3);
    puts("sprite lifecycle, scroll alignment and appearance: PASS");
    return 0;
}
