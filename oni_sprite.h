/* Original Konoko costume pixels in the game's unflipped 8x8 tile space.
 * Paired tiles share a panel so sleeves, glasses and boot seams meet.
 * The guest still places and flips each tile for its current animation. */
enum { ONI_FACE=1, ONI_COLLAR, ONI_JACKET, ONI_BOOTS, ONI_STEP, ONI_STRIDE,
       ONI_BACK, ONI_BACK_COLLAR, ONI_BACK_JACKET, ONI_GLOVE, ONI_GUARD, ONI_PUNCH, ONI_PUNCH_WAIST, ONI_ROLL, ONI_FALL_LEGS, ONI_FALL_BOOTS, ONI_FALL, ONI_FALL_LOWER };

static const char oni_panels[][8][17] = {
    [ONI_FACE]={
        "....p....p......","....pp..php..p..","..p.vphphpv.pv..","...pppppppppv...",
        "..vppvvvppppvv..","...sssssppvvv...","..#l###l##vpv...","..###ss##svppv.."},
    [ONI_COLLAR]={
        "..sssss#ssvpvv..","...ssssssvvp.v..","....#sss#vv.....",".....#ss##......",
        "....#j#s#jH#....","...#Hjj#Hjjj#...","...#jjjzjj#j#...","...#j#zjjj#j#..."},
    [ONI_JACKET]={
        "..#j#jzjj#H#....","..#j#zjjj#j#....","..#H#zjjj#j#....","..#s#jzjj#s#....",
        "...s#HjzH#s.....","....#tHtt#......","....#tjtjt#.....","...#ttj#jt#....."},
    [ONI_BOOTS]={
        "....#jt#.jt#....","....#jt#.#t#....","....#Ht#.#H#....","....#jt#.#t#....",
        "....#jj#.#j#....","....#Hj#.#Hj#...","...#jjj#.#jj#...","..#####..####..."},
    [ONI_STEP]={
        ".....#tj#jt#....","......#tjjt#....","......#tHj#.....",".....#jjt#......",
        "....#Hj#t#......","....#j##Hj#.....","...#jj##jj#.....","...###.####....."},
    [ONI_STRIDE]={
        "....#jt#.jt#....","...#jt#..#tj#...","...#Ht#...#t#...","..#jt#....#Hj#..",
        "..#jj#.....#j#..",".#Hj#......#jj#.","#jjj#......#Hj#.","####.......####."},
    [ONI_BACK]={
        "....p....p......","....pp..php..p..","..p.vphphpv.pv..","...pppppppppv...",
        "..vppvppppppvv..","...vpppvpppvv...","...vpppvvpvpv...","..vvpvvppvvppv.."},
    [ONI_BACK_COLLAR]={
        "...vpvpppvvpvv..","...vv.vpvvvp.v..","....v..vv.vv....",".....##s##......",
        "....#HjjjHH#....","...#jjHHHjjj#...","...#j#jjjj#j#...","...#j#jjjj#j#..."},
    [ONI_BACK_JACKET]={
        "...#j#jjjj#H#...","...#j#Hjjj#j#...","...#H#jjjj#j#...","...#s#jjjj#s#...",
        "....s#HHHH#s....",".....#ttjt#.....",".....#tjjt#.....","....#tj#jt#....."},
    [ONI_GLOVE]={
        "................","....###.........","...#HHj####.....","..#jjjjHjjj##...",
        "..#jjjjjjjjHj#..","...#jj#jjjjjj#..","....###..#jj#...","..........##...."},
    [ONI_GUARD]={
        "....#Hjjjjj#....","....#jzjjjj#....","....#jzjjjj#....","....#jjzjj#.....",
        "....#HjjzH#.....","....#ttHjt#.....","....#jttjt#.....","...#ttj#jt#....."},
    [ONI_PUNCH]={
        "....sssssvv.....","...##sss#vp.....","###HH#ss###.....","jjjjjj##jHj#....",
        "jjjjjjjjH#j#....","####jjjzj#j#....","....#jzjj#j#....","....#zjjj#H#...."},
    [ONI_PUNCH_WAIST]={
        "....#zjjj#s#....","....#jzjj#s#....","....#jjzj##.....","....#HjjzH#.....",
        "....#ttHjt#.....","....#jtjjt#.....","....#tj#jt#.....","...#ttj#jt#....."},
    /* The two tucked heads occupy opposite ends of different roll poses. */
    [ONI_ROLL]={
        "...p..p.p..p....","..pp.hp.vphpp...","..phhpp..vpppp..","ppppppp.ssppvpv.",
        ".vvpppss##svpp..","vvvps#l#l#ssvpv.","..vps###ssssvpp.",".vvpssssss#ssvp."},
    [ONI_FALL_LEGS]={
        "................","................",".....##.........","....#jH##.......",
        "...#jjtjt###....","..#Hj##tttjj##..",".#jj#..##jjjj###",".###.....##jjjjj"},
    [ONI_FALL_BOOTS]={
        "............#jHj","..........##Hjjj",".......###jtjj##",".....##tjjtj##..",
        "...##jtHjj##....","..#Hjj####......",".#jjj#..........",".####..........."},
    [ONI_FALL]={
        ".........p..p...","......##.pphp.p.","....#jssvvphpp..","...#jjssssvpppv.",
        "..#Hj#ss#l#vppv.",".##jj#ss###svpv.","#jjjj#sss#ssvpp.","jjjjj#sss#l#vppv"},
    [ONI_FALL_LOWER]={
        "jjjzjH#ss###vpv.","jjzjjj#ssssvpv..","...#jj#sssvpv...","...#jjj###vv....",
        "...#Hjjj#.......","....#jj#........",".....##.........","................"},
};

#define ONI_PAIR(left,right,panel) [left]=2*(panel),[right]=2*(panel)+1
static const uint8_t oni_tiles[256]={
    ONI_PAIR(0x00,0x01,ONI_FACE), ONI_PAIR(0x60,0x61,ONI_FACE),
    ONI_PAIR(0x40,0x41,ONI_FACE), ONI_PAIR(0x62,0x63,ONI_FACE),
    ONI_PAIR(0x10,0x11,ONI_COLLAR), ONI_PAIR(0x20,0x21,ONI_JACKET),
    ONI_PAIR(0x30,0x31,ONI_BOOTS), ONI_PAIR(0x02,0x03,ONI_STEP),
    ONI_PAIR(0x12,0x13,ONI_STRIDE), ONI_PAIR(0x1e,0x1f,ONI_BACK),
    ONI_PAIR(0x2e,0x2f,ONI_BACK_COLLAR), ONI_PAIR(0x3e,0x3f,ONI_BACK_JACKET),
    ONI_PAIR(0xa2,0xa7,ONI_ROLL), ONI_PAIR(0x47,0x48,ONI_FALL_LEGS),
    ONI_PAIR(0x57,0x58,ONI_FALL_BOOTS), ONI_PAIR(0x49,0x4a,ONI_FALL), ONI_PAIR(0x93,0xd5,ONI_FALL),
    [0x67]=2*ONI_FALL+1, ONI_PAIR(0x59,0x5a,ONI_FALL_LOWER),
    ONI_PAIR(0x90,0x91,ONI_FACE), ONI_PAIR(0x64,0x65,ONI_FACE),
    ONI_PAIR(0xa0,0xa1,ONI_COLLAR), [0x66]=2*ONI_COLLAR,
    ONI_PAIR(0x9e,0x9f,ONI_FACE), ONI_PAIR(0xae,0xaf,ONI_COLLAR),
    ONI_PAIR(0x45,0x46,ONI_FACE), ONI_PAIR(0x77,0x78,ONI_FACE),
    ONI_PAIR(0x07,0x08,ONI_GLOVE), ONI_PAIR(0x16,0x17,ONI_GUARD),
    ONI_PAIR(0x26,0x27,ONI_PUNCH), ONI_PAIR(0x36,0x37,ONI_PUNCH_WAIST),
    ONI_PAIR(0x09,0x0a,ONI_FACE)
};
#undef ONI_PAIR

#include "oni_motion.h"

static int oni_has_tile(uint8_t tile) {
    return oni_tiles[tile] || oni_motion_tiles[tile];
}

static char oni_ink(uint8_t tile,int row,int bit) {
    unsigned entry=oni_tiles[tile];
    if (!entry) return oni_motion_tiles[tile][row*8+7-bit];
    return oni_panels[entry/2][row][(entry&1)*8+7-bit];
}

static unsigned oni_pen(char ink) {
    switch (ink) {
    case '.': return 0;
    case 's': return 3;
    case '#': case 'p': case 'h': case 'v': case 'l': return 1;
    default: return 2;
    }
}

static uint8_t oni_color(char ink,int owner,uint8_t original) {
    switch (ink) {
    case 'v': return 0x14;
    case 'p': return 0x24;
    case 'h': return 0x34;
    case 'l': return 0x21;
    case 'j': return 0x00;
    case 't': return 0x2d;
    case 'H': return 0x10;
    case 'z': return owner==1 ? 0x20 : 0x22;
    default: return original;
    }
}
