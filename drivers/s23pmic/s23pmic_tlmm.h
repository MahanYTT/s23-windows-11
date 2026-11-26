/*
 * s23pmic_tlmm.h - Pure C11 TLMM pin addressing + config/io decoding for the s23pmic read-only probe.
 *
 * Dependency-free (<stdint.h>/<stddef.h> only). The base/stride/offsets are exactly the ones
 * drivers/s23batt/s23batt.c already uses on the phone (tests/c/extract_s23pmic.py extracts them from
 * s23batt.c and s23touch.c and test_s23pmic_intsrc.c checks they are equal):
 *   s23batt.c:518 TLMM_PHYS_BASE 0x0F100000, :519 TLMM_MAP_SIZE 0x100000, :520-521 pin * 0x1000,
 *   :548/552 io register at pin + 4 (bit 0 = input level), :541-542 EGPIO_PRESENT 0x0800 / EGPIO_ENABLE 0x1000.
 * CFG field decode (pull [1:0], func [5:2], oe [9]) is the one s23sensprobe.c:521-527 logs; the drive
 * strength field [8:6] follows public Linux drivers/pinctrl/qcom/pinctrl-msm.c (drv_bit = 6, width 3).
 * TZ-reserved pins mirror s23sensprobe.c:242-248 (GPIO36..39, 50..51, 56..59, 64..65).
 */
#ifndef S23PMIC_TLMM_H
#define S23PMIC_TLMM_H

#include <stdint.h>
#include <stddef.h>

#define S23PMIC_TLMM_PHYS_BASE   0x0F100000ull
#define S23PMIC_TLMM_MAP_SIZE    0x00100000u
#define S23PMIC_TLMM_STRIDE      0x1000u
#define S23PMIC_TLMM_CFG_OFF     0x0u
#define S23PMIC_TLMM_IO_OFF      0x4u

#define S23PMIC_GPIO_SE8_SDA     206u /* ARCH.md:18, s23batt.c:4 */
#define S23PMIC_GPIO_SE8_SCL     207u
#define S23PMIC_GPIO_MAX77705_IRQ 133u /* ARCH.md:18,23; SENSORS_MAP.md:147 */

static inline int s23pmic_tlmm_pin_is_tz_reserved(uint32_t pin)
{
    if (pin >= 36u && pin <= 39u) return 1;
    if (pin == 50u || pin == 51u) return 1;
    if (pin >= 56u && pin <= 59u) return 1;
    if (pin == 64u || pin == 65u) return 1;
    return 0;
}

/* Offset of the pin's CFG register inside the TLMM map. Returns 0 on success, -1 if the pin is
 * TZ-reserved or its io register would lie outside the TLMM map. */
static inline int s23pmic_tlmm_pin_offset(uint32_t pin, uint32_t *off)
{
    if (off == NULL) return -1;
    *off = 0u;
    if (s23pmic_tlmm_pin_is_tz_reserved(pin)) return -1;
    if (pin >= S23PMIC_TLMM_MAP_SIZE / S23PMIC_TLMM_STRIDE) return -1; /* also rules out overflow */
    *off = pin * S23PMIC_TLMM_STRIDE;
    return 0;
}

typedef struct {
    uint32_t pull;          /* 0 none, 1 pull-down, 2 keeper, 3 pull-up (pinctrl-msm) */
    uint32_t func;
    uint32_t drv_ma;        /* (field + 1) * 2 mA */
    uint32_t oe;
    uint32_t egpio_present; /* s23batt.c:542 bit 11 */
    uint32_t egpio_enable;  /* s23batt.c:542 bit 12 */
    uint32_t in;            /* io bit 0 */
    uint32_t out;           /* io bit 1 */
} s23pmic_tlmm_pin_state;

static inline void s23pmic_tlmm_decode(uint32_t cfg, uint32_t io, s23pmic_tlmm_pin_state *s)
{
    if (s == NULL) return;
    s->pull = cfg & 0x3u;
    s->func = (cfg >> 2) & 0xFu;
    s->drv_ma = (((cfg >> 6) & 0x7u) + 1u) * 2u;
    s->oe = (cfg >> 9) & 1u;
    s->egpio_present = (cfg >> 11) & 1u;
    s->egpio_enable = (cfg >> 12) & 1u;
    s->in = io & 1u;
    s->out = (io >> 1) & 1u;
}

#endif /* S23PMIC_TLMM_H */
