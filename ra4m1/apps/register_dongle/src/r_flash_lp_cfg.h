// ============================================================================
// r_flash_lp_cfg.h — FSP r_flash_lp module configuration.
//
// FSP's r_flash_lp.h #includes "r_flash_lp_cfg.h"; on a board set up through
// the FSP Smart Configurator this file is auto-generated into ra_cfg/fsp_cfg/.
// The xiao_ra4m1 board was hand-created (copied from uno_r4) without running
// the configurator for the flash module, so this is a hand-written equivalent
// kept with the app and on the Makefile include path (src/ is in INC).
//
// Settings for register_dongle's use (commissioning blob on the data flash):
//   * data-flash programming   ENABLED  — flash_storage.c erases/writes it
//   * code-flash programming   disabled — we never reprogram code flash
//   * data-flash BGO support   disabled — flash_storage uses blocking mode
//   * parameter checking       follows the global BSP setting
// ============================================================================

#ifndef R_FLASH_LP_CFG_H_
#define R_FLASH_LP_CFG_H_

#define FLASH_LP_CFG_PARAM_CHECKING_ENABLE          (BSP_CFG_PARAM_CHECKING_ENABLE)
#define FLASH_LP_CFG_CODE_FLASH_PROGRAMMING_ENABLE  (0)
#define FLASH_LP_CFG_DATA_FLASH_PROGRAMMING_ENABLE  (1)
#define FLASH_LP_CFG_DATA_FLASH_BGO_SUPPORT_ENABLE  (0)

#endif /* R_FLASH_LP_CFG_H_ */
