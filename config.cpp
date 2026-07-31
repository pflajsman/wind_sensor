// config.cpp — NVS load/default logic (spec §9).

#include "config.h"

static const char *NVS_NAMESPACE = "awning";

void loadConfig(AwningConfig &cfg) {
  Preferences prefs;
  // read-write so first boot can persist the defaults.
  prefs.begin(NVS_NAMESPACE, /*readOnly=*/false);

  cfg.act_thresh  = prefs.getUChar ("act_thresh",  cfgdefault::ACT_THRESH);
  cfg.hb_interval = prefs.getUInt  ("hb_interval", cfgdefault::HB_INTERVAL);
  cfg.act_report  = prefs.getUShort("act_report",  cfgdefault::ACT_REPORT);
  cfg.quiet_mg    = prefs.getUShort("quiet_mg",    cfgdefault::QUIET_MG);
  cfg.quiet_win   = prefs.getUChar ("quiet_win",   cfgdefault::QUIET_WIN);
  cfg.max_active  = prefs.getUShort("max_active",  cfgdefault::MAX_ACTIVE);
  cfg.vbat_ratio  = prefs.getFloat ("vbat_ratio",  cfgdefault::VBAT_RATIO);

  // On first boot the keys do not exist; write them back so they are visible and
  // editable later (e.g. via a serial console or a future OTA config path).
  if (!prefs.isKey("act_thresh"))  prefs.putUChar ("act_thresh",  cfg.act_thresh);
  if (!prefs.isKey("hb_interval")) prefs.putUInt  ("hb_interval", cfg.hb_interval);
  if (!prefs.isKey("act_report"))  prefs.putUShort("act_report",  cfg.act_report);
  if (!prefs.isKey("quiet_mg"))    prefs.putUShort("quiet_mg",    cfg.quiet_mg);
  if (!prefs.isKey("quiet_win"))   prefs.putUChar ("quiet_win",   cfg.quiet_win);
  if (!prefs.isKey("max_active"))  prefs.putUShort("max_active",  cfg.max_active);
  if (!prefs.isKey("vbat_ratio"))  prefs.putFloat ("vbat_ratio",  cfg.vbat_ratio);

  prefs.end();

  DBG("[cfg] act_thresh=%u hb=%us act_report=%us quiet_mg=%u quiet_win=%u max_active=%umin vbat_ratio=%.3f\n",
      cfg.act_thresh, cfg.hb_interval, cfg.act_report, cfg.quiet_mg,
      cfg.quiet_win, cfg.max_active, cfg.vbat_ratio);
}
