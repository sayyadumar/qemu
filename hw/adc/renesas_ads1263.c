#include "qemu/osdep.h"
#include "qemu/log.h"
#include "chardev/char.h"
#include "hw/adc/renesas_ads1263.h"
#include "qom/object.h"

struct Ads1263Chardev {
    Chardev parent;
    uint8_t regs[ADS1263_NUM_REGS];
    uint8_t opcode;
    int remain;
    int32_t sample;
};
typedef struct Ads1263Chardev Ads1263Chardev;

#define TYPE_CHARDEV_ADS1263 "chardev-ads1263"
DECLARE_INSTANCE_CHECKER(Ads1263Chardev, ADS1263_CHARDEV, TYPE_CHARDEV_ADS1263)

static int ads1263_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    Ads1263Chardev *s = ADS1263_CHARDEV(chr);
    uint8_t resp[8]; int resp_len = 0;
    for (int i = 0; i < len; i++) {
        uint8_t b = buf[i];
        if (s->remain == 0) {
            s->opcode = b;
            if ((b & ADS1263_CMD_WREG) == ADS1263_CMD_WREG || (b & ADS1263_CMD_RREG) == ADS1263_CMD_RREG) s->remain = 2;
            else if (b == ADS1263_CMD_RDATA1 || b == 0x14) s->remain = 5;
            else if (b == ADS1263_CMD_RESET) { for (int r = 0; r < ADS1263_NUM_REGS; r++) s->regs[r] = 0x00; s->regs[ADS1263_REG_ID]=0x20; s->regs[ADS1263_REG_POWER]=0x11; s->regs[ADS1263_REG_MODE2]=0x04; s->remain = 1; }
            else s->remain = 1;
            resp[resp_len++] = 0x00;
        } else {
            s->remain--;
            if (s->remain == 0) {
                if ((s->opcode & ADS1263_CMD_RREG) == ADS1263_CMD_RREG) resp[resp_len++] = s->regs[s->opcode & 0x1F];
                else if ((s->opcode & ADS1263_CMD_WREG) == ADS1263_CMD_WREG) { s->regs[s->opcode & 0x1F] = b; resp[resp_len++] = 0x00; }
                else if (s->opcode == ADS1263_CMD_RDATA1 || s->opcode == 0x14) { resp[resp_len++] = s->sample & 0xff; s->sample += 0x00010000; }
                else resp[resp_len++] = 0x00;
            } else if (s->opcode == ADS1263_CMD_RDATA1 || s->opcode == 0x14) {
                if (s->remain == 4) resp[resp_len++] = ADS1263_STATUS_ADC1_RDY;
                else resp[resp_len++] = (s->sample >> (8 * (s->remain - 1))) & 0xff;
            } else resp[resp_len++] = 0x00;
        }
    }
    if (resp_len > 0) qemu_chr_be_write(chr, resp, resp_len);
    return len;
}
static void ads1263_chr_open(Chardev *chr, ChardevBackend *b, bool *be_opened, Error **errp)
{ Ads1263Chardev *s = ADS1263_CHARDEV(chr); memset(s->regs,0,sizeof(s->regs)); s->regs[ADS1263_REG_ID]=0x20; s->regs[ADS1263_REG_POWER]=0x11; s->regs[ADS1263_REG_INTERFACE]=0x05; s->regs[ADS1263_REG_MODE2]=0x04; s->regs[ADS1263_REG_INPMUX]=0x01; s->regs[ADS1263_REG_REFMUX]=0x00; s->remain=0; s->sample=0x10000; *be_opened=false; }
static void char_ads1263_class_init(ObjectClass *oc, const void *d)
{ ChardevClass *cc = CHARDEV_CLASS(oc); cc->open = ads1263_chr_open; cc->chr_write = ads1263_chr_write; }
static const TypeInfo char_ads1263_type_info = { .name = TYPE_CHARDEV_ADS1263, .parent = TYPE_CHARDEV, .instance_size = sizeof(Ads1263Chardev), .class_init = char_ads1263_class_init, };
static void register_types(void) { type_register_static(&char_ads1263_type_info); }
type_init(register_types);
