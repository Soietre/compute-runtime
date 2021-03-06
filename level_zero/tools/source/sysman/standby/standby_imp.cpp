/*
 * Copyright (C) 2019-2020 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "standby_imp.h"

#include "shared/source/helpers/debug_helpers.h"

#include <cmath>

namespace L0 {

ze_result_t StandbyImp::standbyGetProperties(zes_standby_properties_t *pProperties) {
    *pProperties = standbyProperties;
    return ZE_RESULT_SUCCESS;
}

ze_result_t StandbyImp::standbyGetMode(zes_standby_promo_mode_t *pMode) {
    return pOsStandby->getMode(*pMode);
}

ze_result_t StandbyImp::standbySetMode(const zes_standby_promo_mode_t mode) {
    return pOsStandby->setMode(mode);
}

void StandbyImp::init() {
    standbyProperties.type = ZES_STANDBY_TYPE_GLOBAL; // Currently the only defined type
    standbyProperties.onSubdevice = false;
    standbyProperties.subdeviceId = 0;
    this->isStandbyEnabled = pOsStandby->isStandbySupported();
}

StandbyImp::StandbyImp(OsSysman *pOsSysman) {
    pOsStandby = OsStandby::create(pOsSysman);
    UNRECOVERABLE_IF(nullptr == pOsStandby);
    init();
}

StandbyImp::~StandbyImp() {
    delete pOsStandby;
}

} // namespace L0
