/* sound.c -- ALSA mixer controls interface for LFD
 *
 * Copyright (c) 2010 LeapFrog Enterprises Inc.
 *
 * Andrey Yurovsky <ayurovsky@leapfrog.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <getopt.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <alsa/asoundlib.h>

#define _GNU_SOURCE
#include <string.h>

#include "debug.h"
#include "volumeTable.h"

#define CARD_NAME	"default"


#define VOL_DEFAULT	(VOL_STEPS/2 + 1)

int volume_init(void){
	int fdi = -1;
	char buff[4];

	fdi = open("/sys/devices/platform/openlf-didj-volume/adc_reading", O_RDONLY);

	if (fdi != -1){
		read(fdi, buff, sizeof(buff));
		dbprintf("Initial ADC: %d\n", atoi(buff));
		close(fdi);
		return atoi(buff);
	}
	else
	{
		return VOL_DEFAULT;
	}

}
struct playback_dev {
	snd_mixer_elem_t *elem;
	long pmin, pmax, pvol_left, pvol_right;
	uint8_t level_left, level_right;
	char *name;
	uint8_t *vol;
};

enum board_type {
	UNKNOWN = 0,
	CS43L22 = 1,
	};
	
enum board_type	board;

static snd_mixer_t *handle = NULL;

struct playback_dev cs43l22_sp = {
	.name		= "Speaker",
	.vol		= speakerVolume,
	.level_left	= VOL_DEFAULT,
	.level_right	= VOL_DEFAULT,
};

struct playback_dev cs43l22_hp = {
	.name		= "Headphone",
	.vol		= headphoneVolume,
	.level_left	= VOL_DEFAULT,
	.level_right	= VOL_DEFAULT,
};

static uint8_t get_level(long val, uint8_t *vol)
{
	int i;
	
	for (i = 0; i < VOL_STEPS; i++)
		if (val <= vol[i])
			return i;

	return VOL_STEPS - 1;
}

static int set_master_playback_sw(snd_mixer_t *handle, int val)
{
	snd_mixer_selem_id_t *sid;
	snd_mixer_elem_t *elem;

	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, 0);
	snd_mixer_selem_id_set_name(sid, "Master");

	elem = snd_mixer_find_selem(handle, sid);
	if (!elem) {
		dbprintf("can't find simple control \"%s\",%i\n",
				snd_mixer_selem_id_get_name(sid),
				snd_mixer_selem_id_get_index(sid));
		return 1;
	}

	dbprintf("setting master playback: %d\n", val);
	return snd_mixer_selem_set_playback_switch(elem, 0, val);
}

static int get_master_playback_sw(snd_mixer_t *handle, int *val)
{
	snd_mixer_selem_id_t *sid;
	snd_mixer_elem_t *elem;

	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, 0);
	snd_mixer_selem_id_set_name(sid, "Master");

	elem = snd_mixer_find_selem(handle, sid);
	if (!elem) {
		dbprintf("can't find simple control \"%s\",%i\n",
				snd_mixer_selem_id_get_name(sid),
				snd_mixer_selem_id_get_index(sid));
		return 1;
	}

	return snd_mixer_selem_get_playback_switch(elem, 0, val);
}

static int get_playback_info(snd_mixer_t *handle, struct playback_dev *dev)
{
	snd_mixer_selem_id_t *sid;

	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, 0);
	snd_mixer_selem_id_set_name(sid, dev->name);

	dev->elem = snd_mixer_find_selem(handle, sid);
	if (!dev->elem) {
		dbprintf("can't find simple control \"%s\",%i\n",
				snd_mixer_selem_id_get_name(sid),
				snd_mixer_selem_id_get_index(sid));
		return 1;
	}

	if (!snd_mixer_selem_has_playback_volume(dev->elem)) {
		dbprintf("simple control \"%s\",%i doesn't have pvolume\n", 
				snd_mixer_selem_id_get_name(sid),
				snd_mixer_selem_id_get_index(sid));
		return 1;
	}

	snd_mixer_selem_get_playback_volume_range(dev->elem,
			&dev->pmin, &dev->pmax);

	snd_mixer_selem_get_playback_volume(dev->elem,
			SND_MIXER_SCHN_FRONT_LEFT, &dev->pvol_left);
	snd_mixer_selem_get_playback_volume(dev->elem,
			SND_MIXER_SCHN_FRONT_RIGHT, &dev->pvol_right);
	
	dev->level_left = get_level(dev->pvol_left, dev->vol);
	dev->level_right = get_level(dev->pvol_right, dev->vol);

	dbprintf("%s: %ld-%ld %ld,%ld (%u,%u)\n", dev->name, dev->pmin,
			dev->pmax, dev->pvol_left, dev->pvol_right,
			dev->level_left, dev->level_right);

	return 0;
}

static void set_volume(struct playback_dev *dev)
{
	snd_mixer_selem_set_playback_volume(dev->elem,
			SND_MIXER_SCHN_FRONT_LEFT,
			dev->vol[dev->level_left]);

	snd_mixer_selem_set_playback_volume(dev->elem,
			SND_MIXER_SCHN_FRONT_RIGHT,
			dev->vol[dev->level_right]);
}

static int set_playback_level(struct playback_dev *dev, uint8_t level)
{
	if (!handle || !dev)
		return 1;

	if (level > VOL_STEPS)
		level = VOL_STEPS;

	dbprintf("setting \"%s\" volume level to %d\n", dev->name, level);

	if (level == 0) /* we just need to mute the system */
		return set_master_playback_sw(handle, 0);

	/* otherwise we need to set the level and unmute */
	
	dev->level_left = level - 1;
	dev->level_right = level - 1;

	set_volume(dev);
	set_master_playback_sw(handle, 1);

	return 0;
}

static int get_playback_level(struct playback_dev *dev, uint8_t *level)
{
	int ret;
	int pb = 0;

	if (!handle || !dev || !level)
		return 1;

	ret = get_master_playback_sw(handle, &pb);
	if (ret) {
		dbprintf("unable to read master playback switch\n");
		return ret;
	}

	if (pb > 0) {
		ret = get_playback_info(handle, dev);
		if (ret) {
			dbprintf("unable to read \"%s\" volume\n", dev->name);
			return ret;
		}
		*level = dev->level_left + 1;
	} else {
		*level = 0;
	}

	dbprintf("\"%s\" volume level = %d\n", dev->name, *level);
	return 0;
}

int handle_volume(int adc_reading)
{
		int ret;
		int adj_vol = (adc_reading >> 3) & 0x7F;

		ret = set_playback_level(&cs43l22_sp, adj_vol);
		if (ret)
			return ret;

		return set_playback_level(&cs43l22_hp, adj_vol);	
}

/* API functions */

int sound_connect(void)
{
	int err = 0;
	char *name = 0;
	int adc_reading;	

	err = snd_mixer_open(&handle, 0);
	if (err < 0) {
		dbprintf("Mixer open error: %s\n", snd_strerror(err));
		return err;
	}

	err = snd_mixer_attach(handle, CARD_NAME);
	if (err < 0) {
		dbprintf("Mixer attach error: %s\n", snd_strerror(err));
		goto out_connect;
	}

	err = snd_card_get_name(0, (char **)&name);
	if (err < 0) {
		dbprintf("Unable to get card name erro: %s\n",
			snd_strerror(err));
			goto out_connect;
	} else {
		dbprintf("Card name is %s\n", name);
	}

	if (strcasestr(name, "43l22")) { /* cirrus logic 43l22 */
		board = CS43L22;
	} else {	/* unrecognized chip, default to cs43l22 */
		dbprintf("Volume not set for card: %s\n", name);
		board = UNKNOWN;
	}

	err = snd_mixer_selem_register(handle, NULL, NULL);
	if (err < 0) {
		dbprintf("Mixer register error: %s\n", snd_strerror(err));
		goto out_connect;
	}

	err = snd_mixer_load(handle);
	if (err < 0) {
		dbprintf("Mixer load error: %s\n", snd_strerror(err));
		goto out_connect;
	}
	get_playback_info(handle, &cs43l22_hp);
	get_playback_info(handle, &cs43l22_sp);
	adc_reading = volume_init();
	handle_volume(adc_reading);

	return 0;

out_connect:
	snd_mixer_close(handle);
	handle = NULL;
	return err;
}

void sound_disconnect(void)
{
	if (!handle)
		return;
	
	snd_mixer_detach(handle, CARD_NAME);
	snd_mixer_close(handle);
	handle = NULL;
}

int sound_get_volume(void)
{
	uint8_t lsp = 0, lhp = 0;

	get_playback_level(&cs43l22_sp, &lsp);
	get_playback_level(&cs43l22_hp, &lhp);

	if (lsp > lhp)
		return lhp;
	return lsp;
}

void sound_set_volume(int level)
{
	set_playback_level(&cs43l22_sp, level);
	set_playback_level(&cs43l22_hp, level);

}
