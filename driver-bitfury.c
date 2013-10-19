/*
 * device-bitfury.c - device functions for Bitfury chip/board library
 *
 * Copyright (c) 2013 luke-jr
 * Copyright (c) 2013 bitfury
 * Copyright (c) 2013 legkodymov
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
*/

#include "miner.h"
#include <unistd.h>
#include <sha2.h>
#include "libbitfury.h"
#include "util.h"
#include "config.h"

#include <time.h>

#define GOLDEN_BACKLOG 5

struct device_drv bitfury_drv;

// Forward declarations
static void bitfury_disable(struct thr_info* thr);
static bool bitfury_prepare(struct thr_info *thr);
int calc_stat(time_t * stat_ts, time_t stat, struct timeval now);
static void get_options(struct cgpu_info *cgpu);

static void bitfury_detect(void)
{
	int chip_n;
	int i;
	struct cgpu_info *bitfury_info;

	bitfury_info = calloc(1, sizeof(struct cgpu_info));
	bitfury_info->drv = &bitfury_drv;
	bitfury_info->threads = 1;

	applog(LOG_INFO, "INFO: bitfury_detect");
	chip_n = libbitfury_detectChips(bitfury_info->devices);
	if (!chip_n) {
		applog(LOG_WARNING, "No Bitfury chips detected!");
		return;
	} else {
		applog(LOG_WARNING, "BFY: %d chips detected!", chip_n);
	}

	bitfury_info->chip_n = chip_n;
	add_cgpu(bitfury_info);
}

static uint32_t bitfury_checkNonce(struct work *work, uint32_t nonce)
{
	applog(LOG_INFO, "INFO: bitfury_checkNonce");
}

static int bitfury_submitNonce(struct thr_info *thr, struct bitfury_device *device, struct timeval *now, struct work *owork, uint32_t nonce)
{
	if(!submit_nonce(thr, owork, nonce))
		return 0;
	device->nonces[device->current_nonce++] = nonce;
	if(device->current_nonce > 32)
		device->current_nonce = 0;
	device->stat_ts[device->stat_counter++] = now->tv_sec;
	if (device->stat_counter == BITFURY_STAT_N)
		device->stat_counter = 0;

	return 1;
}

static int64_t bitfury_scanHash(struct thr_info *thr)
{
	static struct bitfury_device *devices, *dev; // TODO Move somewhere to appropriate place
	int chip_n;
	int chip;
	uint64_t hashes = 0;
	struct timeval now;
	unsigned char line[2048];
	int short_stat = 10;
	static time_t short_out_t;
	int long_stat = 600;
	static time_t long_out_t;
	int long_long_stat = 60 * 30;
	static time_t long_long_out_t;
	static int first = 1; //TODO Move to detect()
	int i;
	int nonces_cnt;
	struct timespec td;
	struct timespec ts_now;
	static int reinit_cnt = 0;

	devices = thr->cgpu->devices;
	chip_n = thr->cgpu->chip_n;

	if (first) {
		clock_gettime(CLOCK_REALTIME, &ts_now);

		for (i = 0; i < chip_n; i++) {
			devices[i].osc6_bits = devices[i].osc6_bits_setpoint;
			devices[i].osc6_req = devices[i].osc6_bits_setpoint;
		}
		for (i = 0; i < chip_n; i++) {
			devices[i].ts1 = ts_now;
			send_reinit(devices[i].slot, devices[i].fasync, devices[i].osc6_bits);
		}
	}
	first = 0;

	for (chip = 0; chip < chip_n; chip++) {
		dev = &devices[chip];
		dev->job_switched = 0;
		if(!dev->work) {
			dev->work = get_queued(thr->cgpu);
			if (dev->work == NULL) {
				return 0;
			}
			work_to_payload(&(dev->payload), dev->work);
		}
	}

	libbitfury_sendHashData(thr, devices, chip_n);

	cgtime(&now);
	clock_gettime(CLOCK_REALTIME, &ts_now);

	chip = 0;
	for (;chip < chip_n; chip++) {
		nonces_cnt = 0;
		dev = &devices[chip];
		if (dev->job_switched) {
			int j;
			int *res = dev->results;
			struct work *work = dev->work;
			struct work *owork = dev->owork;
			struct work *o2work = dev->o2work;
			for (j = dev->results_n-1; j >= 0; j--) {
				if (owork) {
					nonces_cnt += bitfury_submitNonce(thr, dev, &now, owork, bswap_32(res[j]));
				}
			}
			dev->results_n = 0;
			dev->job_switched = 0;
			if ((dev->old_num > 0) && o2work)
				for(j = 0; j < dev->old_num; j++) {
					nonces_cnt += bitfury_submitNonce(thr, dev, &now, o2work, bswap_32(dev->old_nonce[j]));
				}

			if (dev->future_num > 0)
				for(j=0; j < dev->future_num; j++) {
				        nonces_cnt += bitfury_submitNonce(thr, dev, &now, work, bswap_32(dev->future_nonce[j]));
				}

			if (o2work)
				work_completed(thr->cgpu, o2work);

			dev->o2work = dev->owork;
			dev->owork = dev->work;
			dev->work = NULL;
			hashes += 0xffffffffull * nonces_cnt;
			dev->matching_work += nonces_cnt;
		}

		td = t_diff(dev->ts1, ts_now);
		if(td.tv_sec > 60) {
			if(calc_stat(dev->stat_ts, BITFURY_API_STATS, now) < 150) { // ~1.079 Gh/s @ 300 seconds
				send_shutdown(dev->slot, dev->fasync);
				cgsleep_ms(10);
				send_reinit(dev->slot, dev->fasync, dev->osc6_bits);
				cgsleep_ms(5);
				send_freq(dev->slot, dev->fasync, dev->osc6_bits - 1);
				cgsleep_ms(5);
				send_freq(dev->slot, dev->fasync, dev->osc6_bits);
//				applog(LOG_WARNING, "Chip %d:%d clock reinit to %d bits!", dev->slot, dev->fasync, dev->osc6_bits);
//				if(chip == 0)
//					applog(LOG_WARNING, "Chips reinit to %d bits!", dev->osc6_bits);
			}
			dev->ts1 = ts_now;
		}
	}
//	cgsleep_ms(10);

	return hashes;
}


int calc_stat(time_t * stat_ts, time_t stat, struct timeval now) {
	int j;
	int shares_found = 0;
	for(j = 0; j < BITFURY_STAT_N; j++) {
		if (now.tv_sec - stat_ts[j] < stat) {
			shares_found++;
		}
	}
	return shares_found;
}

static void bitfury_statline_before(char *buf, struct cgpu_info *cgpu)
{
	applog(LOG_INFO, "INFO bitfury_statline_before");
}

static bool bitfury_prepare(struct thr_info *thr)
{
	struct timeval now;
	struct cgpu_info *cgpu = thr->cgpu;

	cgtime(&now);
	get_datestamp(cgpu->init, sizeof(cgpu->init), now.tv_sec);

	get_options(cgpu);

	applog(LOG_INFO, "INFO bitfury_prepare");
	return true;
}

static void bitfury_shutdown(struct thr_info *thr)
{
	int chip_n;
	int i;

	chip_n = thr->cgpu->chip_n;

	applog(LOG_INFO, "INFO bitfury_shutdown");
	libbitfury_shutdownChips(thr->cgpu->devices, chip_n);
}

static void bitfury_disable(struct thr_info *thr)
{
	applog(LOG_INFO, "INFO bitfury_disable");
}

static int bitfury_findChip(struct bitfury_device *devices, int chip_n, int slot, int fs) {
	int n;
	for (n = 0; n < chip_n; n++) {
		if ( (devices[n].slot == slot) && (devices[n].fasync == fs) )
			return n;
	}
	return -1;
}

static void get_options(struct cgpu_info *cgpu)
{
	char buf[BUFSIZ+1];
	char *ptr, *comma, *colon, *colon2;
	size_t max = 0;
	int i, slot, fs, bits, chip, def_bits;

	for(i=0; i<cgpu->chip_n; i++)
		cgpu->devices[i].osc6_bits_setpoint = 54; // this is default value

	if (opt_bitfury_clockbits == NULL) {
		buf[0] = '\0';
		return;
	}

	ptr = opt_bitfury_clockbits;

	do {
		comma = strchr(ptr, ',');
		if (comma == NULL)
			max = strlen(ptr);
		else
			max = comma - ptr;
		if (max > BUFSIZ)
			max = BUFSIZ;
		strncpy(buf, ptr, max);
		buf[max] = '\0';

		if (*buf) {
			colon = strchr(buf, ':');
			if (colon) {
				*(colon++) = '\0';
				colon2 = strchr(colon, ':');
				if (colon2)
					*(colon2++) = '\0';
				if (*buf && *colon && *colon2) {
					slot = atoi(buf);
					fs = atoi(colon);
					bits = atoi(colon2);
					chip = bitfury_findChip(cgpu->devices, cgpu->chip_n, slot, fs);
					if(chip >= 0 && chip < cgpu->chip_n && bits >= 20 && bits <= 56) {
						cgpu->devices[chip].osc6_bits_setpoint = bits;
						applog(LOG_INFO, "Set clockbits: slot=%d chip=%d bits=%d", slot, fs, bits);
					}
				}
			} else {
				def_bits = atoi(buf);
				if(def_bits >= 20 && def_bits <= 56) {
					for(i=0; i<cgpu->chip_n; i++)
						cgpu->devices[i].osc6_bits_setpoint = def_bits;
				}
			}
		}
		if(comma != NULL)
			ptr = ++comma;
	} while (comma != NULL);
}

static double shares_to_ghashes(int shares, int seconds) {
	return ( (double)shares * 4.294967296 ) / ( (double)seconds );

}


static struct api_data *bitfury_api_stats(struct cgpu_info *cgpu)
{
	struct api_data *root = NULL;
	static struct bitfury_device *devices;
	struct timeval now;
	double ts_float;
	struct bitfury_info *info = cgpu->device_data;
	int shares_found, i;
	double ghash, ghash_sum = 0.0;
	char mcw[24];
	uint64_t total_hw = 0;

	devices = cgpu->devices;
	root = api_add_int(root, "chip_n", &(cgpu->chip_n),false);
	cgtime(&now);

	for (i = 0; i < cgpu->chip_n; i++) {
		sprintf(mcw, "clock_bits_%d_%d", devices[i].slot, devices[i].fasync);
		root = api_add_int(root, mcw, &(devices[i].osc6_bits), false);
	}
	for (i = 0; i < cgpu->chip_n; i++) {
		sprintf(mcw, "match_work_count_%d_%d", devices[i].slot, devices[i].fasync);
		root = api_add_uint(root, mcw, &(devices[i].matching_work), false);
	}
	for (i = 0; i < cgpu->chip_n; i++) {
		sprintf(mcw, "hw_errors_%d_%d", devices[i].slot, devices[i].fasync);
		root = api_add_uint(root, mcw, &(devices[i].hw_errors), false);
		total_hw += devices[i].hw_errors;
	}
//	for (i = 0; i < cgpu->chip_n; i++) {
//		sprintf(mcw, "mhz_%d_%d", devices[i].slot, devices[i].fasync);
//		root = api_add_double(root, mcw, &(devices[i].mhz), false);
//	}
	for (i = 0; i < cgpu->chip_n; i++) {
		shares_found = calc_stat(devices[i].stat_ts, BITFURY_API_STATS, now);
		ghash = shares_to_ghashes(shares_found, BITFURY_API_STATS);
		ghash_sum += ghash;
		sprintf(mcw, "ghash_%d_%d", devices[i].slot, devices[i].fasync);
		root = api_add_double(root, mcw, &(ghash), true);
	}
//	for (i = 0; i < cgpu->chip_n; i++) {
//		ts_float = (double)devices[i].ts1.tv_sec + (double)devices[i].ts1.tv_nsec*1e-9;
//		sprintf(mcw, "ts_%d_%d", devices[i].slot, devices[i].fasync);
//		root = api_add_double(root, mcw, &(ts_float), true);
//	}
	api_add_uint64(root, "total_hw", &(total_hw), false);
	api_add_double(root, "total_gh", &(ghash_sum), true);
	ghash_sum /= cgpu->chip_n;
	api_add_double(root, "avg_gh_per_chip", &(ghash_sum), true);

	return root;
}


static
bool bitfury_init(struct thr_info *thr)
{
	return true;
}

struct device_drv bitfury_drv = {
	.dname = "bitfury_gpio",
	.name = "BFY",
	.drv_detect = bitfury_detect,
	.thread_prepare = bitfury_prepare,
	.thread_init = bitfury_init,
	.scanwork = bitfury_scanHash,
	.thread_shutdown = bitfury_shutdown,
	.minerloop = hash_queued_work,
	.get_api_stats = bitfury_api_stats,
};

