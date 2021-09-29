<<<<<<< HEAD:techpack/audio/asoc/codecs/aqt1000/aqt1000-utils.h
/* Copyright (c) 2016-2018, The Linux Foundation. All rights reserved.
=======
/* Copyright (c) 2019, The Linux Foundation. All rights reserved.
>>>>>>> f0ae63cf13ef84f24b6c2a74c663455424d11f9f:arch/arm64/boot/dts/qcom/sdxprairie-cdp-v1.1-cpe.dts
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

<<<<<<< HEAD:techpack/audio/asoc/codecs/aqt1000/aqt1000-utils.h
#ifndef __WCD9XXX_UTILS_H__
#define __WCD9XXX_UTILS_H__

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/regmap.h>

struct regmap *aqt1000_regmap_init(struct device *dev,
				   const struct regmap_config *config);
#endif
=======
/dts-v1/;

#include "sdxprairie-cdp-v1.1-cpe.dtsi"

/ {
	model = "Qualcomm Technologies, Inc. SDXPRAIRIE CDP (CPE-1.1)";
	compatible = "qcom,sdxprairie-cdp",
		"qcom,sdxprairie", "qcom,cdp";
	qcom,board-id = <0x5010101 0x0>;
};


>>>>>>> f0ae63cf13ef84f24b6c2a74c663455424d11f9f:arch/arm64/boot/dts/qcom/sdxprairie-cdp-v1.1-cpe.dts
