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

int main(void) {
    uint8_t saved[512];
    s_ready=1; s_enabled=1;
    g_ram[0x40]=1; g_ram[0x48]=0x80; g_ram[0x4b]=0x80;
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
    g_nes_config.hair[0]=3;
    assert(rcr_sprite_color(HAIR,0,0,6,1,0x0f)==0x1a);
    assert(rcr_sprite_color(HAIR,0,7,6,1,0x0f)==0x0f);
    assert(rcr_sprite_color(HAIR,0,0,6,3,0x26)==0x26);
    assert(rcr_sprite_color(HAIR,0,0,6,1,0x30)==0x30);
    assert(rcr_sprite_color(0,0,0,6,1,0x0f)==0x0f);
    puts("sprite lifecycle, scroll alignment and appearance: PASS");
    return 0;
}
