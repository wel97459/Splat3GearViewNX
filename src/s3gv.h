#ifndef S3GV_H
#define S3GV_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ---- Offsets (Splatoon 3 v11.3.0, from LegoFigure11/Splat3GearView) ----
#define DLC_OFFS            0x3B000u
#define GEARLIST_OFFS       0x2C020u
#define GEARLIST_HEAD       0xBA24BDD8u
#define GEARLIST_CLOTHES    (GEARLIST_HEAD + GEARLIST_OFFS)
#define GEARLIST_SHOES      (GEARLIST_CLOTHES + GEARLIST_OFFS)
#define GACHA_BLOCK         0xBA342D28u

#define GEAR_SIZE           0xB0
#define GACHA_STRUCT_SIZE   0x80
#define GACHA_SEED_OFFSET   0x48
#define GACHA_SEED_SIZE     0x10
#define GACHA_SIZE          0x900

#define GACHA_KEY_NORMAL    0x55663B19u
#define GACHA_KEY_FEST      0xA00259CFu
#define GACHA_KEY_MURCH     0xCCC5FFFBu
#define GACHA_KEY_TITLE     0xD7FD118Du
#define GACHA_KEY_BANNER    0xB4DF951Bu

#define MAX_GEAR 1024

typedef struct {
    uint8_t data[GEAR_SIZE];
} Gear;

typedef struct {
    uint8_t raw[GEAR_SIZE];
    uint8_t gear_type;
    uint16_t id;
    uint16_t exp;
    uint8_t rarity;
    uint8_t primary;
    uint8_t sub_count;
    uint8_t subs[3];
    uint32_t seed;
    const char* name;
    const char* brand;
    const char* rowid;
} GearInfo;

typedef struct {
    uint32_t s0, s1, s2, s3;
    uint8_t type;
} GachaSeed;

// ---- Memory reading (svcDebugActiveProcess on current application) ----
int mem_attach(void);       // attach to current app; 0 on success
void mem_detach(void);      // detach
int mem_read(uint64_t addr, void* buf, size_t size); // 0 on success
int mem_query(uint64_t addr, uint64_t* raddr, uint64_t* rsize, uint32_t* rtype, uint32_t* rperm);
int64_t mem_locate_gacha(void); // heap-scan for the gacha key block; -1 if not found

// ---- Gear parsing ----
bool gear_validate(const uint8_t* d);
void gear_parse(GearInfo* g, const uint8_t* data, uint8_t gear_type);
bool gear_read_list(GearInfo* out, int max, uint64_t base, uint64_t start_pos, uint8_t type,
                    const char** status, int* count);
int64_t gear_locate_list(int type, uint64_t start); // heap-scan for a run of valid gear structs of this type
int64_t gear_locate_inventory(int type, int* out_len, uint64_t start); // short bounded run = owned gear

// ---- Seeds ----
int gacha_read(uint64_t base, GachaSeed* out, int max); // returns count

// ---- Dump ----
bool dump_json(const char* path, const GearInfo* gears, int count);
const char* ability_name(int id);

#endif
