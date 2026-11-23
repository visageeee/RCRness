#pragma once
#include <stdint.h>
void rcr_ws_init(const char *rom_path);
int rcr_ws_arg(const char *key, const char *value);
void rcr_ws_frame(void);
void rcr_wallet_frame(uint32_t *frame, int width);
void rcr_ws_dma(void);
uint8_t rcr_ws_read(uint16_t pc, uint16_t addr, uint8_t value);
int rcr_ws_debug(const char *cmd, int id, const char *json);
uint8_t rcr_font_row(char c, int row);

enum { RCR_SWATCH_COUNT = 6, RCR_HAIR_STYLE_COUNT = 41, RCR_EYE_STYLE_COUNT = 40 };
int rcr_hair_style(int player);
int rcr_eye_style(int player);
const char *rcr_eye_style_name(int style);
const char *rcr_hair_style_name(int style);
void rcr_style_sprite_row(uint8_t tag, uint8_t tile, int row, uint8_t *lo, uint8_t *hi);
const char *rcr_swatch_name(int choice);
uint32_t rcr_swatch_color(int player, int choice, int hair);
void rcr_standing_preview(uint32_t *out, int stride, int x, int y, int player);
void rcr_oam_sprite_write(uint16_t pc, int slot);
void rcr_appearance_dma(uint8_t page, const uint8_t *oam, int repair);
void rcr_native_sprite_tags(uint8_t tags[64]);
uint8_t rcr_shirt_color(int owner, uint8_t original);
uint8_t rcr_sprite_color(uint8_t tag, uint8_t tile, int row, int bit, int pixel, const uint8_t palette[4]);

enum { RCR_MAP_ROOMS = 35 };
int rcr_map_current_room(void);
int rcr_map_objective_room(void);
uint64_t rcr_map_exits(int room);
void rcr_map_name(int room, char name[32]);
void rcr_map_panorama(int room,uint32_t *out,int stride,int width,int height);
