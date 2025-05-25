// SPDX-License-Identifier: GPL-2.0-only
/*
 * This is based on schedutil governor but modified to work with
 * WALT.
 *
 * Support with old kernel min version 4.9
 *
 * Copyright (C) 2016, Intel Corporation
 * Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
 * Copyright (C) 2023, D8G Official.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/binfmts.h>
#include <linux/kthread.h>
#include <trace/events/power.h>
#include <linux/sched/sysctl.h>
#include <linux/version.h>
#include <linux/sched/cpufreq.h>
#include <uapi/linux/sched/types.h>
#include <linux/cpufreq.h>
#include <linux/slab.h>
#include "sched.h"
#ifdef CONFIG_SCHED_WALT
#include "walt.h"
#endif

struct waltgov_tunables {
	struct gov_attr_set	attr_set;
	unsigned int		up_rate_limit_us;
	unsigned int		down_rate_limit_us;
	unsigned int		hispeed_load;
	unsigned int		hispeed_freq;
	unsigned int		rtg_boost_freq;
	unsigned int		target_load_thresh;
	unsigned int		target_load_shift;
	bool			pl;
	bool 				exp_util;
	int			*target_loads;
    int			ntarget_loads;
    spinlock_t		target_loads_lock;
	int			boost;
};

struct waltgov_policy {
	struct cpufreq_policy	*policy;
	u64			last_ws;
	u64			curr_cycles;
	u64			last_cyc_update_time;
	unsigned long		avg_cap;
	struct waltgov_tunables	*tunables;
	struct list_head	tunables_hook;
	unsigned long		hispeed_util;
	unsigned long		rtg_boost_util;
	unsigned long		max;

	raw_spinlock_t		update_lock;
	u64			last_freq_update_time;
	s64			min_rate_limit_ns;
	s64			up_rate_delay_ns;
	s64			down_rate_delay_ns;
	unsigned int		next_freq;
	unsigned int		cached_raw_freq;
	unsigned int		prev_cached_raw_freq;

	/* The next fields are only needed if fast switch cannot be used: */
	struct	irq_work	irq_work;
	struct	kthread_work	work;
	struct	mutex		work_lock;
	struct	kthread_worker	worker;
	struct task_struct	*thread;
	bool			work_in_progress;

	bool			limits_changed;
	bool			need_freq_update;
};

struct waltgov_cpu {
	struct update_util_data	update_util;
	struct waltgov_policy	*wg_policy;
	unsigned int		cpu;
	struct sched_walt_cpu_load walt_load;

	unsigned long		util;
	unsigned int		flags;

	unsigned long		bw_dl;
	unsigned long		min;
	unsigned long		max;
};

static DEFINE_PER_CPU(struct waltgov_cpu, waltgov_cpu);
static unsigned int stale_ns;
static DEFINE_PER_CPU(struct waltgov_tunables *, cached_tunables);

#define DEFAULT_TARGET_LOAD (0)
static int default_target_loads[] = {DEFAULT_TARGET_LOAD};

/************************ Governor internals ***********************/

static bool waltgov_should_update_freq(struct waltgov_policy *wg_policy, u64 time)
{
	s64 delta_ns;

	if (!cpufreq_can_do_remote_dvfs(wg_policy->policy))
		return false;

	if (unlikely(wg_policy->limits_changed)) {
		wg_policy->limits_changed = false;
		wg_policy->need_freq_update = true;
		return true;
	}

	/*
	 * No need to recalculate next freq for min_rate_limit_us
	 * at least. However we might still decide to further rate
	 * limit once frequency change direction is decided, according
	 * to the separate rate limits.
	 */

	delta_ns = time - wg_policy->last_freq_update_time;
	return delta_ns >= wg_policy->min_rate_limit_ns;
}

static inline bool use_pelt(void)
{
#ifdef CONFIG_SCHED_WALT
	return false;
#else
	return true;
#endif
}

static bool waltgov_up_down_rate_limit(struct waltgov_policy *wg_policy, u64 time,
				     unsigned int next_freq)
{
	s64 delta_ns;

	delta_ns = time - wg_policy->last_freq_update_time;

	if (next_freq > wg_policy->next_freq &&
	    delta_ns < wg_policy->up_rate_delay_ns)
		return true;

	if (next_freq < wg_policy->next_freq &&
	    delta_ns < wg_policy->down_rate_delay_ns)
		return true;

	return false;
}

static bool waltgov_update_next_freq(struct waltgov_policy *wg_policy, u64 time,
						unsigned int next_freq)

{
	if (wg_policy->next_freq == next_freq)
		return false;

	if (waltgov_up_down_rate_limit(wg_policy, time, next_freq)) {
		/* Restore cached freq as next_freq is not changed */
		wg_policy->cached_raw_freq = wg_policy->prev_cached_raw_freq;
		return false;
	}

	wg_policy->next_freq = next_freq;
	wg_policy->last_freq_update_time = time;

	return true;
}

static unsigned long freq_to_util(struct waltgov_policy *wg_policy,
				  unsigned int freq)
{
	return mult_frac(wg_policy->max, freq,
			 wg_policy->policy->cpuinfo.max_freq);
}

#define KHZ 1000
static void waltgov_track_cycles(struct waltgov_policy *wg_policy,
				unsigned int prev_freq,
				u64 upto)
{
	u64 delta_ns, cycles;
	u64 next_ws = wg_policy->last_ws + sched_ravg_window;

	if (use_pelt())
		return;

	upto = min(upto, next_ws);
	/* Track cycles in current window */
	delta_ns = upto - wg_policy->last_cyc_update_time;
	delta_ns *= prev_freq;
	do_div(delta_ns, (NSEC_PER_SEC / KHZ));
	cycles = delta_ns;
	wg_policy->curr_cycles += cycles;
	wg_policy->last_cyc_update_time = upto;
}

static void waltgov_calc_avg_cap(struct waltgov_policy *wg_policy, u64 curr_ws,
				unsigned int prev_freq)
{
	u64 last_ws = wg_policy->last_ws;
	unsigned int avg_freq;

	if (use_pelt())
		return;

	BUG_ON(curr_ws < last_ws);
	if (curr_ws <= last_ws)
		return;

	/* If we skipped some windows */
	if (curr_ws > (last_ws + sched_ravg_window)) {
		avg_freq = prev_freq;
		/* Reset tracking history */
		wg_policy->last_cyc_update_time = curr_ws;
	} else {
		waltgov_track_cycles(wg_policy, prev_freq, curr_ws);
		avg_freq = wg_policy->curr_cycles;
		avg_freq /= sched_ravg_window / (NSEC_PER_SEC / KHZ);
	}
	wg_policy->avg_cap = freq_to_util(wg_policy, avg_freq);
	wg_policy->curr_cycles = 0;
	wg_policy->last_ws = curr_ws;
}
static void waltgov_fast_switch(struct waltgov_policy *wg_policy, u64 time,
			      unsigned int next_freq)
{
	struct cpufreq_policy *policy = wg_policy->policy;
	unsigned int cpu;

	if (!waltgov_update_next_freq(wg_policy, time, next_freq))
		return;

	waltgov_track_cycles(wg_policy, wg_policy->policy->cur, time);
	next_freq = cpufreq_driver_fast_switch(policy, next_freq);
	if (!next_freq)
		return;

	policy->cur = next_freq;

	if (trace_cpu_frequency_enabled()) {
		for_each_cpu(cpu, policy->cpus)
			trace_cpu_frequency(next_freq, cpu);
	}
}

static void waltgov_deferred_update(struct waltgov_policy *wg_policy, u64 time,
				  unsigned int next_freq)
{
	if (use_pelt())
		wg_policy->work_in_progress = true;

#ifdef CONFIG_SCHED_WALT
	walt_irq_work_queue(&wg_policy->irq_work);
#else
	sched_irq_work_queue(&wg_policy->irq_work);
#endif
}

#define TARGET_LOAD 80
static inline unsigned long walt_map_util_freq(unsigned long util,
					struct waltgov_policy *wg_policy,
					unsigned long cap, int cpu)
{
	unsigned long fmax = wg_policy->policy->cpuinfo.max_freq;
	unsigned int shift = wg_policy->tunables->target_load_shift;

	if (util >= wg_policy->tunables->target_load_thresh &&
	    cpu_util_rt(cpu) < (cap >> 2))
		return max(
			(fmax + (fmax >> shift)) * util,
			(fmax + (fmax >> 2)) * wg_policy->tunables->target_load_thresh
			)/cap;
	return (fmax + (fmax >> 2)) * util / cap;
}

/**
 * get_next_freq - Compute a new frequency for a given cpufreq policy.
 * @wg_policy: game_walt policy object to compute the new frequency for.
 * @util: Current CPU utilization.
 * @max: CPU capacity.
 *
 * If the utilization is frequency-invariant, choose the new frequency to be
 * proportional to it, that is
 *
 * next_freq = C * max_freq * util / max
 *
 * Otherwise, approximate the would-be frequency-invariant utilization by
 * util_raw * (curr_freq / max_freq) which leads to
 *
 * next_freq = C * curr_freq * util_raw / max
 *
 * Take C = 1.25 for the frequency tipping point at (util / max) = 0.8.
 *
 * The lowest driver-supported frequency which is equal or greater than the raw
 * next_freq (as calculated above) is returned, subject to policy min/max and
 * cpufreq driver limitations.
 */
static unsigned int get_next_freq(struct waltgov_policy *wg_policy,
				  unsigned long util, unsigned long max)
{
	struct cpufreq_policy *policy = wg_policy->policy;
	unsigned int freq;
 
	if (arch_scale_freq_invariant())
		freq = policy->cpuinfo.max_freq;
	else
		/*
		 * Apply a 25% margin so that we select a higher frequency than
		 * the current one before the CPU is fully busy:
		 */
		freq = policy->cur + (policy->cur >> 2);

	unsigned int idx, l_freq, h_freq;
	freq = (freq + (freq >> 2)) * util / max;

	if (freq == wg_policy->cached_raw_freq && !wg_policy->need_freq_update)
		return wg_policy->next_freq;

	wg_policy->need_freq_update = false;
	wg_policy->cached_raw_freq = freq;
	l_freq = cpufreq_driver_resolve_freq(policy, freq);
	idx = cpufreq_frequency_table_target(policy, freq, CPUFREQ_RELATION_H);
	h_freq = policy->freq_table[idx].frequency;
	h_freq = clamp(h_freq, policy->min, policy->max);
	if (l_freq <= h_freq || l_freq == policy->min)
		return l_freq;
	/*
	 * Use the frequency step below if the calculated frequency is <20%
	 * higher than it.
	 */
	if (mult_frac(100, freq - h_freq, l_freq - h_freq) < 20)
		return h_freq;
	return l_freq;
}

#ifdef CONFIG_SCHED_WALT
static unsigned long waltgov_get_util(struct waltgov_cpu *sg_cpu)
{
	struct rq *rq = cpu_rq(sg_cpu->cpu);
	unsigned long max = arch_scale_cpu_capacity(NULL, sg_cpu->cpu);

	sg_cpu->max = max;
	sg_cpu->bw_dl = cpu_bw_dl(rq);

	return stune_util(sg_cpu->cpu, 0, &sg_cpu->walt_load);
}
#else
static unsigned long waltgov_get_util(struct waltgov_cpu *sg_cpu)
{
	struct rq *rq = cpu_rq(sg_cpu->cpu);

	unsigned long util_cfs = cpu_util_cfs(rq);
	unsigned long max = arch_scale_cpu_capacity(NULL, sg_cpu->cpu);

	sg_cpu->max = max;
	sg_cpu->bw_dl = cpu_bw_dl(rq);

	util = cpu_util_freq_walt(wg_cpu->cpu, &wg_cpu->walt_load);
	return uclamp_rq_util_with(rq, util, NULL);
}
#endif

#define NL_RATIO 75
#define DEFAULT_HISPEED_LOAD 85
#define DEFAULT_CPU0_RTG_BOOST_FREQ 1000000
#define DEFAULT_CPU4_RTG_BOOST_FREQ 0
static int find_target_boost(unsigned long util, struct waltgov_policy *wg_policy,
				unsigned long *min_util)
{
	int i, ret;
	unsigned long flags;

	spin_lock_irqsave(&wg_policy->tunables->target_loads_lock, flags);
	for (i = 0; i < wg_policy->tunables->ntarget_loads - 1 &&
				util >= wg_policy->tunables->target_loads[i+1]; i += 2)
		;
	ret = wg_policy->tunables->target_loads[i];
	if (i == 0)
		*min_util = 0;
	else
		*min_util = wg_policy->tunables->target_loads[i-1];

	spin_unlock_irqrestore(&wg_policy->tunables->target_loads_lock, flags);

	return ret;
}
#define DEFAULT_TARGET_LOAD_THRESH 1024
#define DEFAULT_TARGET_LOAD_SHIFT 4
#ifdef CONFIG_SCHED_WALT
static void waltgov_walt_adjust(struct waltgov_cpu *wg_cpu, unsigned long cpu_util,
				unsigned long nl, unsigned long *util,
				unsigned long *max)
{
	struct waltgov_policy *wg_policy = wg_cpu->wg_policy;
	bool is_migration = wg_cpu->flags & SCHED_CPUFREQ_INTERCLUSTER_MIG;
	bool is_hiload;
	unsigned long min_util;
	int target_boost;
	unsigned long pl = wg_cpu->walt_load.pl;

	if (use_pelt())
		return;

	target_boost = 100 + find_target_boost(*util, wg_policy, &min_util);
	*util = mult_frac(*util, target_boost, 100);
	*util = max(*util, min_util);

	is_hiload = (cpu_util >= mult_frac(wg_policy->avg_cap,
					   wg_policy->tunables->hispeed_load,
					   100));

	if (is_hiload && !is_migration)
		*util = max(*util, wg_policy->hispeed_util);

	if (is_hiload && nl >= mult_frac(cpu_util, NL_RATIO, 100))
		*util = *max;

	if (wg_policy->tunables->pl && wg_cpu->walt_load.pl > *util)
		*util = (*util + wg_cpu->walt_load.pl) / 2;
}
#endif

static inline void ignore_dl_rate_limit(struct waltgov_cpu *wg_cpu, struct waltgov_policy *wg_policy)
{
	if (cpu_bw_dl(cpu_rq(wg_cpu->cpu)) > wg_cpu->bw_dl)
		wg_policy->limits_changed = true;
}

static inline unsigned long target_util(struct waltgov_policy *wg_policy,
				  unsigned int freq)
{
	unsigned long util;

	util = freq_to_util(wg_policy, freq);

#ifdef CONFIG_SCHED_WALT
	if (wg_policy->max == min_max_possible_capacity &&
		util >= wg_policy->tunables->target_load_thresh)
		util = mult_frac(util, 94, 100);
	else
#endif
		util = mult_frac(util, TARGET_LOAD, 100);

	return util;
}

static void waltgov_update_single(struct update_util_data *hook, u64 time,
				unsigned int flags)
{
	struct waltgov_cpu *wg_cpu = container_of(hook, struct waltgov_cpu, update_util);
	struct waltgov_policy *wg_policy = wg_cpu->wg_policy;
	struct cpufreq_policy *policy = wg_policy->policy;
	unsigned long util, max, hs_util, boost_util;
	unsigned int next_f, j;
	int boost = wg_policy->tunables->boost;

	if (!wg_policy->tunables->pl && flags & SCHED_CPUFREQ_PL)
		return;

	if (!waltgov_should_update_freq(wg_policy, time))
		return;

	wg_cpu->util = util = waltgov_get_util(wg_cpu);
	max = wg_cpu->max;
	wg_cpu->flags = flags;

	if (wg_policy->max != max) {
		wg_policy->max = max;
		hs_util = target_util(wg_policy,
				       wg_policy->tunables->hispeed_freq);
		wg_policy->hispeed_util = hs_util;

		boost_util = target_util(wg_policy,
				    wg_policy->tunables->rtg_boost_freq);
		wg_policy->rtg_boost_util = boost_util;
	}

	waltgov_calc_avg_cap(wg_policy, wg_cpu->walt_load.ws,
			   wg_policy->policy->cur);

	trace_sugov_util_update(wg_cpu->cpu, wg_cpu->util,
				wg_policy->avg_cap, max, wg_cpu->walt_load.nl,
				wg_cpu->walt_load.pl,
				wg_cpu->walt_load.rtgb_active, flags);

#ifdef CONFIG_SCHED_WALT
	for_each_cpu(j, policy->cpus) {
		struct waltgov_cpu *j_wg_cpu = &per_cpu(waltgov_cpu, j);
		unsigned long j_util, j_nl;

		j_util = j_wg_cpu->util;
		j_nl = j_wg_cpu->walt_load.nl;
		if (boost) {
			j_util = mult_frac(j_util, boost + 100, 100);
			j_nl = mult_frac(j_nl, boost + 100, 100);
		}

		waltgov_walt_adjust(wg_cpu, j_util, j_nl, &util, &max);
	}
#endif
	next_f = get_next_freq(wg_policy, util, max);

	/*
	 * This code runs under rq->lock for the target CPU, so it won't run
	 * concurrently on two different CPUs for the same target and it is not
	 * necessary to acquire the lock in the fast switch case.
	 */
	if (wg_policy->policy->fast_switch_enabled) {
		waltgov_fast_switch(wg_policy, time, next_f);
	} else {
		raw_spin_lock(&wg_policy->update_lock);
		waltgov_deferred_update(wg_policy, time, next_f);
		raw_spin_unlock(&wg_policy->update_lock);
	}
}

static unsigned int waltgov_next_freq_shared(struct waltgov_cpu *wg_cpu, u64 time)
{
	struct waltgov_policy *wg_policy = wg_cpu->wg_policy;
	struct cpufreq_policy *policy = wg_policy->policy;
	unsigned long util = 0, max = 1;
	unsigned int j;
	int boost = wg_policy->tunables->boost;

	for_each_cpu(j, policy->cpus) {
		struct waltgov_cpu *j_wg_cpu = &per_cpu(waltgov_cpu, j);
		unsigned long j_util, j_max, j_nl;

		/*
		 * If the util value for all CPUs in a policy is 0, just using >
		 * will result in a max value of 1. WALT stats can later update
		 * the aggregated util value, causing get_next_freq() to compute
		 * freq = max_freq * 1.25 * (util / max) for nonzero util,
		 * leading to spurious jumps to fmax.
		 */
		j_util = j_wg_cpu->util;
		j_nl = j_wg_cpu->walt_load.nl;
		j_max = j_wg_cpu->max;
		if (boost) {
			j_util = mult_frac(j_util, boost + 100, 100);
			j_nl = mult_frac(j_nl, boost + 100, 100);
		}

		if (j_util * max >= j_max * util) {
			util = j_util;
			max = j_max;
		}
#ifdef CONFIG_SCHED_WALT
		waltgov_walt_adjust(j_wg_cpu, j_util, j_nl, &util, &max);
#endif
	}

	return get_next_freq(wg_policy, util, max);
}

static void 
waltgov_update_freq(struct update_util_data *hook, u64 time,
				unsigned int flags)
{
	struct waltgov_cpu *wg_cpu = container_of(hook, struct waltgov_cpu, update_util);
	struct waltgov_policy *wg_policy = wg_cpu->wg_policy;
	unsigned long hs_util, boost_util;
	unsigned int next_f;

	if (!wg_policy->tunables->pl && flags & SCHED_CPUFREQ_PL)
		return;

	wg_cpu->util = waltgov_get_util(wg_cpu);
	wg_cpu->flags = flags;
	raw_spin_lock(&wg_policy->update_lock);

	if (wg_policy->max != wg_cpu->max) {
		wg_policy->max = wg_cpu->max;
		hs_util = target_util(wg_policy,
					wg_policy->tunables->hispeed_freq);
		wg_policy->hispeed_util = hs_util;

		boost_util = target_util(wg_policy,
				    wg_policy->tunables->rtg_boost_freq);
		wg_policy->rtg_boost_util = boost_util;
	}

	waltgov_calc_avg_cap(wg_policy, wg_cpu->walt_load.ws,
			   wg_policy->policy->cur);

	trace_sugov_util_update(wg_cpu->cpu, wg_cpu->util, wg_policy->avg_cap,
				wg_cpu->max, wg_cpu->walt_load.nl,
				wg_cpu->walt_load.pl,
				wg_cpu->walt_load.rtgb_active, flags);

	if (waltgov_should_update_freq(wg_policy, time) &&
	    !(flags & SCHED_CPUFREQ_CONTINUE)) {
		next_f = waltgov_next_freq_shared(wg_cpu, time);

		if (wg_policy->policy->fast_switch_enabled)
			waltgov_fast_switch(wg_policy, time, next_f);
		else
			waltgov_deferred_update(wg_policy, time, next_f);
	}

out:
	raw_spin_unlock(&wg_policy->update_lock);
}

static void waltgov_work(struct kthread_work *work)
{
	struct waltgov_policy *wg_policy = container_of(work, struct waltgov_policy, work);
	unsigned int freq;
	unsigned long flags;

	raw_spin_lock_irqsave(&wg_policy->update_lock, flags);
	freq = wg_policy->next_freq;
	if (use_pelt())
		wg_policy->work_in_progress = false;
	waltgov_track_cycles(wg_policy, wg_policy->policy->cur,
			   ktime_get_ns());
	raw_spin_unlock_irqrestore(&wg_policy->update_lock, flags);

	mutex_lock(&wg_policy->work_lock);
	__cpufreq_driver_target(wg_policy->policy, freq, CPUFREQ_RELATION_L);
	mutex_unlock(&wg_policy->work_lock);
}

static void waltgov_irq_work(struct irq_work *irq_work)
{
	struct waltgov_policy *wg_policy;

	wg_policy = container_of(irq_work, struct waltgov_policy, irq_work);

	kthread_queue_work(&wg_policy->worker, &wg_policy->work);
}

/************************** sysfs interface ************************/

static inline struct waltgov_tunables *to_waltgov_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct waltgov_tunables, attr_set);
}

static DEFINE_MUTEX(min_rate_lock);

static void update_min_rate_limit_ns(struct waltgov_policy *wg_policy)
{
	mutex_lock(&min_rate_lock);
	wg_policy->min_rate_limit_ns = min(wg_policy->up_rate_delay_ns,
					   wg_policy->down_rate_delay_ns);
	mutex_unlock(&min_rate_lock);
}

static ssize_t up_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct waltgov_tunables *tunables = to_waltgov_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->up_rate_limit_us);
}

static ssize_t down_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct waltgov_tunables *tunables = to_waltgov_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->down_rate_limit_us);
}

static ssize_t up_rate_limit_us_store(struct gov_attr_set *attr_set,
				      const char *buf, size_t count)
{
	struct waltgov_tunables *tunables = to_waltgov_tunables(attr_set);
	struct waltgov_policy *wg_policy;
	unsigned int rate_limit_us;

	if (task_is_zygote(current))
		return count;

	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;

	tunables->up_rate_limit_us = rate_limit_us;

	list_for_each_entry(wg_policy, &attr_set->policy_list, tunables_hook) {
		wg_policy->up_rate_delay_ns = rate_limit_us * NSEC_PER_USEC;
		update_min_rate_limit_ns(wg_policy);
	}

	return count;
}

static ssize_t down_rate_limit_us_store(struct gov_attr_set *attr_set,
					const char *buf, size_t count)
{
	struct waltgov_tunables *tunables = to_waltgov_tunables(attr_set);
	struct waltgov_policy *wg_policy;
	unsigned int rate_limit_us;

	if (task_is_zygote(current))
		return count;

	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;

	tunables->down_rate_limit_us = rate_limit_us;

	list_for_each_entry(wg_policy, &attr_set->policy_list, tunables_hook) {
		wg_policy->down_rate_delay_ns = rate_limit_us * NSEC_PER_USEC;
		update_min_rate_limit_ns(wg_policy);
	}

	return count;
}

static struct governor_attr up_rate_limit_us = __ATTR_RW(up_rate_limit_us);
static struct governor_attr down_rate_limit_us = __ATTR_RW(down_rate_limit_us);

static ssize_t hispeed_load_show(struct gov_attr_set *attr_set, char *buf)
{
	struct waltgov_tunables *tunables = to_waltgov_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->hispeed_load);
}

static ssize_t hispeed_load_store(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
	struct waltgov_tunables *tunables = to_waltgov_tunables(attr_set);

	if (kstrtouint(buf, 10, &tunables->hispeed_load))
		return -EINVAL;

	tunables->hispeed_load = min(100U, tunables->hispeed_load);

	return count;
}

static ssize_t hispeed_freq_show(struct gov_attr_set *attr_set, char *buf)
{
	struct waltgov_tunables *tunables = to_waltgov_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->hispeed_freq);
}

static ssize_t hispeed_freq_store(struct gov_attr_set *attr_set,
					const char *buf, size_t count)
{
	struct waltgov_tunables *tunables = to_waltgov_tunables(attr_set);
	unsigned int val;
	struct waltgov_policy *wg_policy;
	unsigned long hs_util;
	unsigned long flags;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	tunables->hispeed_freq = val;
	list_for_each_entry(wg_policy, &attr_set->policy_list, tunables_hook) {
		raw_spin_lock_irqsave(&wg_policy->update_lock, flags);
		hs_util = target_util(wg_policy,
					wg_policy->tunables->hispeed_freq);
		wg_policy->hispeed_util = hs_util;
		raw_spin_unlock_irqrestore(&wg_policy->update_lock, flags);
	}

	return count;
}

static ssize_t rtg_boost_freq_show(struct gov_attr_set *attr_set, char *buf)
{
	struct waltgov_tunables *tunables = to_waltgov_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->rtg_boost_freq);
}

static ssize_t rtg_boost_freq_store(struct gov_attr_set *attr_set,
				    const char *buf, size_t count)
{
	struct waltgov_tunables *tunables = to_waltgov_tunables(attr_set);
	unsigned int val;
	struct waltgov_policy *wg_policy;
	unsigned long rtg_boost_util;
	unsigned long flags;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	tunables->rtg_boost_freq = val;
	list_for_each_entry(wg_policy, &attr_set->policy_list, tunables_hook) {
		raw_spin_lock_irqsave(&wg_policy->update_lock, flags);
		rtg_boost_util = target_util(wg_policy,
					  wg_policy->tunables->rtg_boost_freq);
		wg_policy->rtg_boost_util = rtg_boost_util;
		raw_spin_unlock_irqrestore(&wg_policy->update_lock, flags);
	}

	return count;
}

static ssize_t pl_show(struct gov_attr_set *attr_set, char *buf)
{
	struct waltgov_tunables *tunables = to_waltgov_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->pl);
}

static ssize_t pl_store(struct gov_attr_set *attr_set, const char *buf,
				   size_t count)
{
	struct waltgov_tunables *tunables = to_waltgov_tunables(attr_set);

	if (kstrtobool(buf, &tunables->pl))
		return -EINVAL;

	return count;
}

static unsigned int *get_tokenized_data(const char *buf, int *num_tokens)
{
	const char *cp;
	char *ptr, *ptr_bak, *token;
	int i = 0, len = 0;
	int ntokens = 1;
	int *tokenized_data;
	int err = -EINVAL;

	cp = buf;
	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	if (!(ntokens & 0x1))
		goto err;

	tokenized_data = kmalloc_array(ntokens, sizeof(int), GFP_KERNEL);
	if (!tokenized_data) {
		err = -ENOMEM;
		goto err;
	}

	len = strlen(buf) + 1;
	ptr = ptr_bak = kmalloc(len, GFP_KERNEL);
	if (!ptr) {
		kfree(tokenized_data);
		err = -ENOMEM;
		goto err;
	}

	memcpy(ptr, buf, len);
	token = strsep(&ptr, " :");
	while (token != NULL) {
		if (kstrtoint(token, 10, &tokenized_data[i++]))
			goto err_kfree;
		token = strsep(&ptr, " :");
	}

	if (i != ntokens)
		goto err_kfree;
	kfree(ptr_bak);

	*num_tokens = ntokens;
	return tokenized_data;

err_kfree:
	kfree(ptr_bak);
	kfree(tokenized_data);
err:
	return ERR_PTR(err);
}

static ssize_t target_loads_show(struct gov_attr_set *attr_set, char *buf)
{
	int i;
	int tmp;
	ssize_t ret = 0;
	unsigned long flags;
	struct waltgov_policy *wg_policy;
	struct waltgov_tunables *tunables = to_waltgov_tunables(attr_set);

	spin_lock_irqsave(&tunables->target_loads_lock, flags);

	for (i = 0; i < tunables->ntarget_loads; i++) {
		list_for_each_entry(wg_policy, &attr_set->policy_list, tunables_hook) {
			if (i & 0x1)
				tmp = map_util_freq(tunables->target_loads[i],
							wg_policy->policy->cpuinfo.max_freq,
							wg_policy->max);
			else
				tmp = tunables->target_loads[i];
		}
		ret += scnprintf(buf + ret, PAGE_SIZE - ret, "%d%s",
					tmp, i & 0x1 ? ":" : " ");
	}
	scnprintf(buf + ret - 1, PAGE_SIZE - (ret - 1), "\n");
	spin_unlock_irqrestore(&tunables->target_loads_lock, flags);

	return ret;
}

static ssize_t target_loads_store(struct gov_attr_set *attr_set, const char *buf,
				   size_t count)
{
	int i;
	int ntokens;
	unsigned long util;
	unsigned long flags;
	struct waltgov_policy *wg_policy;
	int *new_target_loads = NULL;
	struct waltgov_tunables *tunables = to_waltgov_tunables(attr_set);

	new_target_loads = get_tokenized_data(buf, &ntokens);
	if (IS_ERR(new_target_loads))
		return PTR_ERR_OR_ZERO(new_target_loads);

	spin_lock_irqsave(&tunables->target_loads_lock, flags);
	if (tunables->target_loads != default_target_loads)
		kfree(tunables->target_loads);

	for (i = 0; i < ntokens; i++) {
		list_for_each_entry(wg_policy, &attr_set->policy_list, tunables_hook) {
			if (i % 2) {
				util = target_util(wg_policy, new_target_loads[i]);
				new_target_loads[i] = util;
			}
		}

	}

	tunables->target_loads = new_target_loads;
	tunables->ntarget_loads = ntokens;
	spin_unlock_irqrestore(&tunables->target_loads_lock, flags);

	return count;
}

static ssize_t boost_show(struct gov_attr_set *attr_set, char *buf)
{
	struct waltgov_tunables *tunables = to_waltgov_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%d\n", tunables->boost);
}

static ssize_t boost_store(struct gov_attr_set *attr_set, const char *buf,
				   size_t count)
{
	struct waltgov_tunables *tunables = to_waltgov_tunables(attr_set);
	struct waltgov_policy *wg_policy;
	int val;
	unsigned long hs_util;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	tunables->boost = val;
	list_for_each_entry(wg_policy, &attr_set->policy_list, tunables_hook) {
		struct rq *rq = cpu_rq(wg_policy->policy->cpu);
		unsigned long flags;

		raw_spin_lock_irqsave(&rq->lock, flags);
		hs_util = target_util(wg_policy,
					wg_policy->tunables->hispeed_freq);
		wg_policy->hispeed_util = hs_util;
		raw_spin_unlock_irqrestore(&rq->lock, flags);
	}
	return count;
}

static ssize_t exp_util_show(struct gov_attr_set *attr_set, char *buf)
{
	struct waltgov_tunables *tunables = to_waltgov_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->exp_util);
}

static ssize_t exp_util_store(struct gov_attr_set *attr_set, const char *buf,
				   size_t count)
{
	struct waltgov_tunables *tunables = to_waltgov_tunables(attr_set);

	if (kstrtobool(buf, &tunables->exp_util))
		return -EINVAL;

	return count;
}

#define WALTGOV_ATTR_RW(_name)						\
static struct governor_attr _name =					\
__ATTR(_name, 0644, show_##_name, store_##_name)			\

#define show_attr(name)							\
static ssize_t show_##name(struct gov_attr_set *attr_set, char *buf)	\
{									\
	struct waltgov_tunables *tunables = to_waltgov_tunables(attr_set);	\
	return scnprintf(buf, PAGE_SIZE, "%lu\n", tunables->name);	\
}									\

#define store_attr(name)						\
static ssize_t store_##name(struct gov_attr_set *attr_set,		\
				const char *buf, size_t count)		\
{									\
	struct waltgov_tunables *tunables = to_waltgov_tunables(attr_set);	\
										\
	if (kstrtouint(buf, 10, &tunables->name))			\
		return -EINVAL;						\
									\
	return count;							\
}									\

show_attr(target_load_thresh);
store_attr(target_load_thresh);
show_attr(target_load_shift);
store_attr(target_load_shift);

static struct governor_attr hispeed_load = __ATTR_RW(hispeed_load);
static struct governor_attr hispeed_freq = __ATTR_RW(hispeed_freq);
static struct governor_attr rtg_boost_freq = __ATTR_RW(rtg_boost_freq);
static struct governor_attr pl = __ATTR_RW(pl);
static struct governor_attr target_loads = __ATTR_RW(target_loads);
static struct governor_attr boost = __ATTR_RW(boost);
static struct governor_attr exp_util = __ATTR_RW(exp_util);
WALTGOV_ATTR_RW(target_load_thresh);
WALTGOV_ATTR_RW(target_load_shift);

static struct attribute *waltgov_attributes[] = {
	&up_rate_limit_us.attr,
	&down_rate_limit_us.attr,
	&hispeed_load.attr,
	&hispeed_freq.attr,
	&rtg_boost_freq.attr,
	&pl.attr,
	&target_loads.attr,
	&boost.attr,
	&exp_util.attr,
	&target_load_thresh.attr,
	&target_load_shift.attr,
	NULL
};

static struct kobj_type waltgov_tunables_ktype = {
	.default_attrs	= waltgov_attributes,
	.sysfs_ops	= &governor_sysfs_ops,
};

/********************** cpufreq governor interface *********************/

static struct cpufreq_governor walt_gov;

static struct waltgov_policy *waltgov_policy_alloc(struct cpufreq_policy *policy)
{
	struct waltgov_policy *wg_policy;

	wg_policy = kzalloc(sizeof(*wg_policy), GFP_KERNEL);
	if (!wg_policy)
		return NULL;

	wg_policy->policy = policy;
	raw_spin_lock_init(&wg_policy->update_lock);
	return wg_policy;
}

static void waltgov_policy_free(struct waltgov_policy *wg_policy)
{
	kfree(wg_policy);
}

static int waltgov_kthread_create(struct waltgov_policy *wg_policy)
{
	struct task_struct *thread;
	struct sched_param param = { .sched_priority = MAX_USER_RT_PRIO / 2 };
	struct cpufreq_policy *policy = wg_policy->policy;
	int ret;

	/* kthread only required for slow path */
	if (policy->fast_switch_enabled)
		return 0;

	kthread_init_work(&wg_policy->work, waltgov_work);
	kthread_init_worker(&wg_policy->worker);
	thread = kthread_create(kthread_worker_fn, &wg_policy->worker,
				"waltgov:%d",
				cpumask_first(policy->related_cpus));
	if (IS_ERR(thread)) {
		pr_err("failed to create waltgov thread: %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	ret = sched_setscheduler_nocheck(thread, SCHED_FIFO, &param);
	if (ret) {
		kthread_stop(thread);
		pr_warn("%s: failed to set SCHED_FIFO\n", __func__);
		return ret;
	}

	wg_policy->thread = thread;
	kthread_bind_mask(thread, policy->related_cpus);
	init_irq_work(&wg_policy->irq_work, waltgov_irq_work);
	mutex_init(&wg_policy->work_lock);

	wake_up_process(thread);

	return 0;
}

static void waltgov_kthread_stop(struct waltgov_policy *wg_policy)
{
	/* kthread only required for slow path */
	if (wg_policy->policy->fast_switch_enabled)
		return;

	kthread_flush_worker(&wg_policy->worker);
	kthread_stop(wg_policy->thread);
	mutex_destroy(&wg_policy->work_lock);
}

static void waltgov_tunables_save(struct cpufreq_policy *policy,
		struct waltgov_tunables *tunables)
{
	int cpu;
	struct waltgov_tunables *cached = per_cpu(cached_tunables, policy->cpu);

	if (!cached) {
		cached = kzalloc(sizeof(*tunables), GFP_KERNEL);
		if (!cached)
			return;

		for_each_cpu(cpu, policy->related_cpus)
			per_cpu(cached_tunables, cpu) = cached;
	}

	cached->pl = tunables->pl;
	cached->hispeed_load = tunables->hispeed_load;
	cached->rtg_boost_freq = tunables->rtg_boost_freq;
	cached->hispeed_freq = tunables->hispeed_freq;
	cached->up_rate_limit_us = tunables->up_rate_limit_us;
	cached->down_rate_limit_us = tunables->down_rate_limit_us;
	cached->boost = tunables->boost;
	cached->exp_util = tunables->exp_util;
	cached->target_load_thresh = tunables->target_load_thresh;
	cached->target_load_shift = tunables->target_load_shift;
}

static void waltgov_tunables_restore(struct cpufreq_policy *policy)
{
	struct waltgov_policy *wg_policy = policy->governor_data;
	struct waltgov_tunables *tunables = wg_policy->tunables;
	struct waltgov_tunables *cached = per_cpu(cached_tunables, policy->cpu);

	if (!cached)
		return;

	tunables->pl = cached->pl;
	tunables->hispeed_load = cached->hispeed_load;
	tunables->rtg_boost_freq = cached->rtg_boost_freq;
	tunables->hispeed_freq = cached->hispeed_freq;
	tunables->up_rate_limit_us = cached->up_rate_limit_us;
	tunables->down_rate_limit_us = cached->down_rate_limit_us;
	tunables->boost	= cached->boost;
	tunables->exp_util = cached->exp_util;
	tunables->target_load_thresh = cached->target_load_thresh;
	tunables->target_load_shift = cached->target_load_shift;
}

static int waltgov_init(struct cpufreq_policy *policy)
{
	struct waltgov_policy *wg_policy;
	struct waltgov_tunables *tunables;
	int ret = 0;

	/* State should be equivalent to EXIT */
	if (policy->governor_data)
		return -EBUSY;

	cpufreq_enable_fast_switch(policy);

	if (policy->fast_switch_possible && !policy->fast_switch_enabled)
		BUG_ON(1);

	wg_policy = waltgov_policy_alloc(policy);
	if (!wg_policy) {
		ret = -ENOMEM;
		goto disable_fast_switch;
	}

	ret = waltgov_kthread_create(wg_policy);
	if (ret)
		goto free_wg_policy;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (!tunables) {
		ret = -ENOMEM;
		goto stop_kthread;
	}

	tunables->up_rate_limit_us = 500;
	tunables->down_rate_limit_us = 2000;
	gov_attr_set_init(&tunables->attr_set, &wg_policy->tunables_hook);
	tunables->hispeed_load = DEFAULT_HISPEED_LOAD;
	spin_lock_init(&tunables->target_loads_lock);
	tunables->target_loads = default_target_loads;
	tunables->ntarget_loads = ARRAY_SIZE(default_target_loads);
	tunables->target_load_thresh = DEFAULT_TARGET_LOAD_THRESH;
	tunables->target_load_shift = DEFAULT_TARGET_LOAD_SHIFT;

	switch (policy->cpu) {
	default:
	case 0:
		tunables->rtg_boost_freq = DEFAULT_CPU0_RTG_BOOST_FREQ;
		break;
	case 4:
		tunables->rtg_boost_freq = DEFAULT_CPU4_RTG_BOOST_FREQ;
		break;
	}

	policy->governor_data = wg_policy;
	wg_policy->tunables = tunables;

	stale_ns = sched_ravg_window + (sched_ravg_window >> 3);

	waltgov_tunables_restore(policy);

	ret = kobject_init_and_add(&tunables->attr_set.kobj, &waltgov_tunables_ktype,
				   get_governor_parent_kobj(policy), "%s",
				   walt_gov.name);
	if (ret)
		goto fail;

	return 0;

fail:
	kobject_put(&tunables->attr_set.kobj);
	policy->governor_data = NULL;
	kfree(tunables);
stop_kthread:
	waltgov_kthread_stop(wg_policy);
free_wg_policy:
	waltgov_policy_free(wg_policy);
disable_fast_switch:
	cpufreq_disable_fast_switch(policy);

	pr_err("initialization failed (error %d)\n", ret);
	return ret;
}

static void waltgov_exit(struct cpufreq_policy *policy)
{
	struct waltgov_policy *wg_policy = policy->governor_data;
	struct waltgov_tunables *tunables = wg_policy->tunables;
	unsigned int count;

	count = gov_attr_set_put(&tunables->attr_set, &wg_policy->tunables_hook);
	policy->governor_data = NULL;
	if (!count) {
		waltgov_tunables_save(policy, tunables);
		kfree(tunables);
	}

	waltgov_kthread_stop(wg_policy);
	waltgov_policy_free(wg_policy);
	cpufreq_disable_fast_switch(policy);
}

static int waltgov_start(struct cpufreq_policy *policy)
{
	struct waltgov_policy *wg_policy = policy->governor_data;
	unsigned int cpu;

	wg_policy->up_rate_delay_ns =
		wg_policy->tunables->up_rate_limit_us * NSEC_PER_USEC;
	wg_policy->down_rate_delay_ns =
		wg_policy->tunables->down_rate_limit_us * NSEC_PER_USEC;
	update_min_rate_limit_ns(wg_policy);
	wg_policy->last_freq_update_time	= 0;
	wg_policy->next_freq			= 0;
	wg_policy->work_in_progress		= false;
	wg_policy->limits_changed		= false;
	wg_policy->need_freq_update		= false;
	wg_policy->cached_raw_freq		= 0;
	wg_policy->prev_cached_raw_freq		= 0;

	for_each_cpu(cpu, policy->cpus) {
		struct waltgov_cpu *wg_cpu = &per_cpu(waltgov_cpu, cpu);

		memset(wg_cpu, 0, sizeof(*wg_cpu));
		wg_cpu->cpu			= cpu;
		wg_cpu->wg_policy		= wg_policy;
	}

	for_each_cpu(cpu, policy->cpus) {
		struct waltgov_cpu *wg_cpu = &per_cpu(waltgov_cpu, cpu);

		cpufreq_add_update_util_hook(cpu, &wg_cpu->update_util,
					     policy_is_shared(policy) ?
							waltgov_update_freq :
							waltgov_update_single);
	}

	return 0;
}

static void waltgov_stop(struct cpufreq_policy *policy)
{
	struct waltgov_policy *wg_policy = policy->governor_data;
	unsigned int cpu;

	for_each_cpu(cpu, policy->cpus)
		cpufreq_remove_update_util_hook(cpu);

	synchronize_rcu();

	if (!policy->fast_switch_enabled) {
		irq_work_sync(&wg_policy->irq_work);
		kthread_cancel_work_sync(&wg_policy->work);
	}
}
static void waltgov_limits(struct cpufreq_policy *policy)
{
	struct waltgov_policy *wg_policy = policy->governor_data;
	unsigned long flags, now;
	unsigned int freq;

	if (!policy->fast_switch_enabled) {
		mutex_lock(&wg_policy->work_lock);
		raw_spin_lock_irqsave(&wg_policy->update_lock, flags);
		waltgov_track_cycles(wg_policy, wg_policy->policy->cur,
				   ktime_get_ns());
		raw_spin_unlock_irqrestore(&wg_policy->update_lock, flags);
		cpufreq_policy_apply_limits(policy);
		mutex_unlock(&wg_policy->work_lock);
	} else {
		raw_spin_lock_irqsave(&wg_policy->update_lock, flags);
		freq = policy->cur;
		now = ktime_get_ns();

		/*
		 * cpufreq_driver_resolve_freq() has a clamp, so we do not need
		 * to do any sort of additional validation here.
		 */
		freq = cpufreq_driver_resolve_freq(policy, freq);
		wg_policy->cached_raw_freq = freq;
		waltgov_fast_switch(wg_policy, now, freq);
		raw_spin_unlock_irqrestore(&wg_policy->update_lock, flags);
	}

	wg_policy->limits_changed = true;
}

static struct cpufreq_governor walt_gov = {
	.name			= "walt",
	.owner			= THIS_MODULE,
	.dynamic_switching	= true,
	.init			= waltgov_init,
	.exit			= waltgov_exit,
	.start			= waltgov_start,
	.stop			= waltgov_stop,
	.limits			= waltgov_limits,
};

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_WALT
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &walt_gov;
}
#endif

static int __init waltgov_register(void)
{
	return cpufreq_register_governor(&walt_gov);
}
fs_initcall(waltgov_register);

