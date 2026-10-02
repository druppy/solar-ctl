/*
 * Native-build LVGL configuration for inv_ctl.
 *
 * Only the values that matter for parity with the Yocto (wrynose meta-oe
 * lvgl_9.4.0) build are set here; lv_conf_internal.h fills in every other
 * default. Keep these in sync with:
 *   layers/meta-openembedded/meta-oe/recipes-graphics/lvgl/files/defconfig
 *   .../files/fbdev.cfg  .../files/sdl.cfg
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* LVGL .S files self-define __ASSEMBLY__ before pulling in the conf chain;
 * C typedefs must not reach the assembler (mirrors lv_conf_template.h). */
#ifndef __ASSEMBLY__
#include <stdint.h>
#endif

#define LV_COLOR_DEPTH 32               /* same as the Yocto defconfig */
#define LV_MEM_SIZE (256U * 1024U)      /* same as the Yocto defconfig */

#define LV_USE_LOG 1
#define LV_LOG_PRINTF 1

#define LV_USE_LINUX_FBDEV 1            /* fbdev parity with the target */
#define LV_USE_SDL 1                    /* laptop windowed mode */

#endif /* LV_CONF_H */
