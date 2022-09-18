/*
 * memcg_reclaim_policy.h
 *
 * Copyright(C) 2020 Huawei Technologies Co., Ltd. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef _MEMCG_POLICY_INTERNAL_H
#define _MEMCG_POLICY_INTERNAL_H

struct mem_cgroup;
struct seqfile;
struct pglist_data;

unsigned long reclaim_all_anon_memcg(struct pglist_data *pgdat,
		struct mem_cgroup *memcg);
unsigned long reclaim_all_anon_memcg_prelaunch(struct pglist_data *pgdat,
		struct mem_cgroup *memcg);
inline bool get_ec_app_start_flag_value(void);
inline bool get_eswap_switch_value(void);

#endif
