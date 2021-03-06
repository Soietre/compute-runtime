/*
 * Copyright (C) 2019-2020 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "level_zero/tools/source/sysman/standby/linux/os_standby_imp.h"

namespace L0 {

const std::string LinuxStandbyImp::standbyModeFile("power/rc6_enable");

bool LinuxStandbyImp::isStandbySupported(void) {
    if (ZE_RESULT_SUCCESS == pSysfsAccess->canRead(standbyModeFile)) {
        return true;
    } else {
        return false;
    }
}

ze_result_t LinuxStandbyImp::getMode(zes_standby_promo_mode_t &mode) {
    int currentMode = -1;
    ze_result_t result = pSysfsAccess->read(standbyModeFile, currentMode);
    if (ZE_RESULT_SUCCESS != result) {
        return result;
    }
    if (standbyModeDefault == currentMode) {
        mode = ZES_STANDBY_PROMO_MODE_DEFAULT;
    } else if (standbyModeNever == currentMode) {
        mode = ZES_STANDBY_PROMO_MODE_NEVER;
    } else {
        result = ZE_RESULT_ERROR_UNKNOWN;
    }
    return result;
}

ze_result_t LinuxStandbyImp::setMode(zes_standby_promo_mode_t mode) {
    // standbyModeFile is not writable.
    // Mode cannot be set from L0.
    // To set the mode, user must reload
    // the i915 module and set module parameter
    // enable_rc6 appropriately.
    return ZE_RESULT_ERROR_UNSUPPORTED_FEATURE;
}

LinuxStandbyImp::LinuxStandbyImp(OsSysman *pOsSysman) {
    LinuxSysmanImp *pLinuxSysmanImp = static_cast<LinuxSysmanImp *>(pOsSysman);

    pSysfsAccess = &pLinuxSysmanImp->getSysfsAccess();
}

OsStandby *OsStandby::create(OsSysman *pOsSysman) {
    LinuxStandbyImp *pLinuxStandbyImp = new LinuxStandbyImp(pOsSysman);
    return static_cast<OsStandby *>(pLinuxStandbyImp);
}

} // namespace L0
