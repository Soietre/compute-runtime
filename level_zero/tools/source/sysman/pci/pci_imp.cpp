/*
 * Copyright (C) 2019-2020 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "pci_imp.h"

#include "shared/source/helpers/basic_math.h"
#include "shared/source/helpers/debug_helpers.h"
#include "shared/source/helpers/string.h"

#include <cstring>
namespace L0 {

//
// While computing the PCIe bandwidth, also consider that due to 8b/10b encoding
// in PCIe gen1 and gen2  real bandwidth will be reduced by 20%,
// And in case of gen3 and above due to 128b/130b encoding real bandwidth is
// reduced by approx 1.54% as compared to theoretical bandwidth.
// In below method, get real PCIe speed in pcieSpeedWithEnc in Mega bits per second
// pcieSpeedWithEnc = maxLinkSpeedInGt * (Gigabit to Megabit) * Encoding =
//                               maxLinkSpeedInGt * 1000 * Encoding
//
int64_t convertPcieSpeedFromGTsToBs(double maxLinkSpeedInGt) {
    double pcieSpeedWithEnc;
    if ((maxLinkSpeedInGt == 16) || (maxLinkSpeedInGt == 8)) {
        pcieSpeedWithEnc = maxLinkSpeedInGt * 1000 * 128 / 130;
    } else if ((maxLinkSpeedInGt == 5) || (maxLinkSpeedInGt == 2.5)) {
        pcieSpeedWithEnc = maxLinkSpeedInGt * 1000 * 8 / 10;
    } else {
        pcieSpeedWithEnc = 0;
    }

    //
    // PCIE speed we got above is in Mega bits per second
    //  Convert that speed in bytes/second.
    //  Now, because 1Mb/s = (1000*1000)/8 bytes/second = 125000 bytes/second
    //
    pcieSpeedWithEnc = pcieSpeedWithEnc * 125000;
    return static_cast<int64_t>(pcieSpeedWithEnc);
}

ze_result_t PciImp::pciStaticProperties(zes_pci_properties_t *pProperties) {
    *pProperties = pciProperties;
    return ZE_RESULT_SUCCESS;
}

ze_result_t PciImp::pciGetInitializedBars(uint32_t *pCount, zes_pci_bar_properties_t *pProperties) {
    uint32_t pciBarPropertiesSize = static_cast<uint32_t>(pciBarProperties.size());
    uint32_t numToCopy = std::min(*pCount, pciBarPropertiesSize);
    if (0 == *pCount || *pCount > pciBarPropertiesSize) {
        *pCount = pciBarPropertiesSize;
    }
    if (nullptr != pProperties) {
        for (uint32_t i = 0; i < numToCopy; i++) {
            pProperties[i] = *pciBarProperties[i];
        }
    }
    return ZE_RESULT_SUCCESS;
}

void PciImp::init() {
    if (pOsPci == nullptr) {
        pOsPci = OsPci::create(pOsSysman);
    }
    UNRECOVERABLE_IF(nullptr == pOsPci);
    std::string bdf;
    pOsPci->getPciBdf(bdf);
    if (bdf.empty()) {
        pciProperties.address.domain = 0;
        pciProperties.address.bus = 0;
        pciProperties.address.device = 0;
        pciProperties.address.function = 0;
    } else {
        sscanf(bdf.c_str(), "%04x:%02x:%02x.%d",
               &pciProperties.address.domain, &pciProperties.address.bus,
               &pciProperties.address.device, &pciProperties.address.function);
    }

    int32_t maxLinkWidth = -1, gen = -1;
    int64_t maxBandWidth = -1;
    double maxLinkSpeed = 0;
    pOsPci->getMaxLinkSpeed(maxLinkSpeed);
    pOsPci->getMaxLinkWidth(maxLinkWidth);
    maxBandWidth = maxLinkWidth * convertPcieSpeedFromGTsToBs(maxLinkSpeed);
    if (maxBandWidth == 0) {
        pciProperties.maxSpeed.maxBandwidth = -1;
    } else {
        pciProperties.maxSpeed.maxBandwidth = maxBandWidth;
    }
    pciProperties.maxSpeed.width = maxLinkWidth;
    pOsPci->getLinkGen(gen);
    pciProperties.maxSpeed.gen = gen;
    pOsPci->initializeBarProperties(pciBarProperties);
}

PciImp::~PciImp() {
    for (zes_pci_bar_properties_t *pProperties : pciBarProperties) {
        delete pProperties;
        pProperties = nullptr;
    }
    if (nullptr != pOsPci) {
        delete pOsPci;
        pOsPci = nullptr;
    }
}
} // namespace L0
