/*
	Copyright (C) 2014 - 2016 CurlyMo

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <assert.h>
#include <sys/stat.h>
#ifndef _WIN32
	#include <unistd.h>
	#ifdef __mips__
		#define __USE_UNIX98
	#endif
#endif

#include "../../core/pilight.h"
#include "../../core/common.h"
#include "../../core/dso.h"
#include "../../core/log.h"
#include "../../core/eventpool.h"
#include "../../core/binary.h"
#include "../../core/gc.h"
#include "../../core/json.h"
#ifndef _WIN32
	#include "../../../wiringx/wiringX.h"
#endif
#include "../protocol.h"
#include "bh1750.h"

#if !defined(__FreeBSD__) && !defined(_WIN32) && !defined(__sun)
typedef struct data_t {
	char *name;

	unsigned int id;
	char path[PATH_MAX];
	int fd;
	int interval;

	uv_timer_t *timer_req;

	struct data_t *next;
} data_t;

static struct data_t *data = NULL;

static void *reason_code_received_free(void *param) {
	struct reason_code_received_t *data = param;
	FREE(data);
	return NULL;
}

static void thread(uv_work_t *req) {
	struct data_t *settings = req->data;

	wiringXI2CWrite(settings->fd,0x01);
	wiringXI2CWrite(settings->fd,0x10);
	usleep (0.5 * 1000000);
	int raw = wiringXI2CReadReg16(settings->fd, 0x00);
	wiringXI2CWrite(settings->fd,0x00);
	int lux = (int) (((raw & 0xff00)>>8 | (raw & 0x00ff)<<8 )) / 1.2 ;

	/* LM75
	int raw = wiringXI2CReadReg16(settings->fd, 0x00);
	raw = ((raw << 8) & 0xFF00) + (raw >> 8);
	float temp = (float)((raw / 32.0) / 8.0);
	*/

	struct reason_code_received_t *data = MALLOC(sizeof(struct reason_code_received_t));
	if(data == NULL) {
		OUT_OF_MEMORY
	}
	snprintf(data->message, 1024, "{\"id\":%d,\"illuminance\":%i}", settings->id, lux);
	strncpy(data->origin, "receiver", 255);
	data->protocol = bh1750->id;
	if(strlen(pilight_uuid) > 0) {
		data->uuid = pilight_uuid;
	} else {
		data->uuid = NULL;
	}
	data->repeat = 1;
	eventpool_trigger(REASON_CODE_RECEIVED, reason_code_received_free, data);

	return;
}

static void *addDevice(int reason, void *param) {
	struct JsonNode *jdevice = NULL;
	struct JsonNode *jprotocols = NULL;
	struct JsonNode *jid = NULL;
	struct JsonNode *jchild = NULL;
	struct data_t *node = NULL;
	char *stmp = NULL;
	int match = 0, interval = 10;
	double itmp = 0.0;

	if(param == NULL) {
		return NULL;
	}

	if((jdevice = json_first_child(param)) == NULL) {
		return NULL;
	}

	if((jprotocols = json_find_member(jdevice, "protocol")) != NULL) {
		jchild = json_first_child(jprotocols);
		while(jchild) {
			if(strcmp(bh1750->id, jchild->string_) == 0) {
				match = 1;
				break;
			}
			jchild = jchild->next;
		}
	}

	if(match == 0) {
		return NULL;
	}

	match = 0;
	if((node = MALLOC(sizeof(struct data_t)))== NULL) {
		OUT_OF_MEMORY
	}
	node->id = 0;
	
	if((jid = json_find_member(jdevice, "id"))) {
		jchild = json_first_child(jid);
		while(jchild) {
			if(json_find_number(jchild, "id", &itmp) == 0) {
				node->id = (int)itmp;
				match++;
			}
			if(json_find_string(jchild, "i2c-path", &stmp) == 0) {
				strcpy(node->path, stmp);
				match++;
			}
			jchild = jchild->next;
		}
	}

	if(match != 2) {
		FREE(node);
		return NULL;
	}

	if(json_find_number(jdevice, "poll-interval", &itmp) == 0) {
		interval = (int)round(itmp);
	}
		
	node->fd = wiringXI2CSetup(node->path, node->id);

	if(node->fd <= 0) {
		logprintf(LOG_ERR, "bh1750: error connecting to i2c bus: %s", node->path);
	}

	if((node->name = MALLOC(strlen(jdevice->key)+1)) == NULL) {
		OUT_OF_MEMORY
	}
	strcpy(node->name, jdevice->key);

	node->interval = interval;

	node->next = data;
	data = node;

	if((node->timer_req = MALLOC(sizeof(uv_timer_t))) == NULL) {
		OUT_OF_MEMORY
	}	
	
	node->timer_req->data = node;
	uv_timer_init(uv_default_loop(), node->timer_req);
	assert(node->interval > 0);
	uv_timer_start(node->timer_req, (void (*)(uv_timer_t *))thread, node->interval*1000, node->interval*1000);

	return NULL;
}

static void gc(void) {
	struct data_t *tmp = NULL;
	while(data) {
		tmp = data;
		if(data->name != NULL) {
			FREE(data->name);
		}
		if(data->fd > 0) {
			close(data->fd);
		}
		data = data->next;
		FREE(tmp);
	}
	if(data != NULL) {
		FREE(data);
	}
}
#endif

#if !defined(MODULE) && !defined(_WIN32) && !defined(__sun)
__attribute__((weak))
#endif
void bh1750Init(void) {

	protocol_register(&bh1750);
	protocol_set_id(bh1750, "bh1750");
	protocol_device_add(bh1750, "bh1750", "Rohm I2C Ambient Light Sensor");
	bh1750->devtype = WEATHER;
	bh1750->hwtype = SENSOR;
	bh1750->multipleId = 0;

	options_add(&bh1750->options, 'i', "id", OPTION_HAS_VALUE, DEVICES_ID, JSON_STRING, NULL, "0x[0-9a-f]{2}");
	options_add(&bh1750->options, 'e', "illuminance", OPTION_HAS_VALUE, DEVICES_VALUE, JSON_NUMBER, NULL, "^[0-9]{1}$");
	options_add(&bh1750->options, 'd', "i2c-path", OPTION_HAS_VALUE, DEVICES_ID, JSON_STRING, NULL, "^/dev/i2c-[0-9]{1,2}$");
	options_add(&bh1750->options, 0, "poll-interval", OPTION_HAS_VALUE, DEVICES_SETTING, JSON_NUMBER, (void *)10, "[0-9]");
	options_add(&bh1750->options, 0, "show-illuminance", OPTION_HAS_VALUE, GUI_SETTING, JSON_NUMBER, (void *)1, "^[10]{1}$");

#if !defined(__FreeBSD__) && !defined(_WIN32) && !defined(__sun)
	bh1750->gc=&gc;

	eventpool_callback(REASON_DEVICE_ADDED, addDevice);
#endif
}

#if defined(MODULE) && !defined(_WIN32) && !defined(__sun)
void compatibility(struct module_t *module) {
	module->name = "bh1750";
	module->version = "3.0";
	module->reqversion = "7.0";
	module->reqcommit = "94";
}

void init(void) {
	bh1750Init();
}
#endif
