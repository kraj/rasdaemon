/*
 * Copyright (C) 2013 Mauro Carvalho Chehab <mchehab@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/poll.h>
#include "libtrace/kbuffer.h"
#include "libtrace/event-parse.h"
#include "ras-mc-handler.h"
#include "ras-aer-handler.h"
#include "ras-mce-handler.h"
#include "ras-record.h"
#include "ras-logger.h"

/*
 * Polling time, if read() doesn't block. Currently, trace_pipe_raw never
 * blocks on read(). So, we need to sleep for a while, to avoid spending
 * too much CPU cycles. A fix for it is expected for 3.10.
 */
#define POLLING_TIME 3

/* Test for a little-endian machine */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	#define ENDIAN KBUFFER_ENDIAN_LITTLE
#else
	#define ENDIAN KBUFFER_ENDIAN_BIG
#endif

static int get_debugfs_dir(char *tracing_dir, size_t len)
{
	FILE *fp;
	char line[MAX_PATH + 1 + 256];
	char *p, *type, *dir;

	fp = fopen("/proc/mounts","r");
	if (!fp) {
		log(ALL, LOG_INFO, "Can't open /proc/mounts");
		return errno;
	}

	do {
		if (!fgets(line, sizeof(line), fp))
			break;

		p = strtok(line, " \t");
		if (!p)
			break;

		dir = strtok(NULL, " \t");
		if (!dir)
			break;

		type = strtok(NULL, " \t");
		if (!type)
			break;

		if (!strcmp(type, "debugfs")) {
			fclose(fp);
			strncpy(tracing_dir, dir, len - 1);
			tracing_dir[len - 1] = '\0';
			return 0;
		}
	} while(1);

	fclose(fp);
	log(ALL, LOG_INFO, "Can't find debugfs\n");
	return ENOENT;
}

static int open_trace(struct ras_events *ras, char *name, int flags)
{
	char fname[MAX_PATH + 1];

	strcpy(fname, ras->tracing);
	strcat(fname, "/");
	strcat(fname, name);

	return open(fname, flags);
}

static int get_tracing_dir(struct ras_events *ras)
{
	char		fname[MAX_PATH + 1];
	int		rc, has_instances = 0;
	DIR		*dir;
	struct dirent	*entry;

	get_debugfs_dir(ras->debugfs, sizeof(ras->debugfs));

	strcpy(fname, ras->debugfs);
	strcat(fname, "/tracing");
	dir = opendir(fname);
	if (!dir)
		return -1;

	for (entry = readdir(dir); entry; entry = readdir(dir)) {
		if (strstr(entry->d_name, "instances")) {
			has_instances = 1;
			break;
		}
	}
	closedir(dir);

	strcpy(ras->tracing, ras->debugfs);
	strcat(ras->tracing, "/tracing");
	if (has_instances) {
		strcat(ras->tracing, "/instances/" TOOL_NAME);
		rc = mkdir(ras->tracing, S_IRWXU);
		if (rc < 0 && errno != EEXIST) {
			log(ALL, LOG_INFO,
			    "Unable to create " TOOL_NAME " instance at %s\n",
			    ras->tracing);
			return -1;
		}
	}
	return 0;
}

/*
 * Tracing enable/disable code
 */
static int __toggle_ras_mc_event(struct ras_events *ras,
				 char *group, char *event, int enable)
{
	int fd, rc;
	char fname[MAX_PATH + 1];

	snprintf(fname, sizeof(fname), "%s%s:%s\n",
		 enable ? "" : "!",
		 group, event);

	/* Enable RAS events */
	fd = open_trace(ras, "set_event", O_RDWR | O_APPEND);
	if (fd < 0) {
		log(ALL, LOG_WARNING, "Can't open set_event\n");
		return errno;
	}

	rc = write(fd, fname,strlen(fname));
	if (rc < 0) {
		log(ALL, LOG_WARNING, "Can't write to set_event\n");
		close(fd);
		return rc;
	}
	close(fd);
	if (!rc) {
		log(ALL, LOG_WARNING, "Nothing was written on set_event\n");
		return EIO;
	}

	log(ALL, LOG_INFO, "%s:%s event %s\n",
	    group, event,
	    enable ? "enabled" : "disabled");

	return 0;
}

int toggle_ras_mc_event(int enable)
{
	struct ras_events *ras;
	int rc = 0;

	ras = calloc(1, sizeof(*ras));
	if (!ras) {
		log(TERM, LOG_ERR, "Can't allocate memory for ras struct\n");
		return errno;
	}

	rc = get_tracing_dir(ras);
	if (rc < 0) {
		log(TERM, LOG_ERR, "Can't locate a mounted debugfs\n");
		goto free_ras;
	}

	rc = __toggle_ras_mc_event(ras, "ras", "mc_event", enable);

#ifdef HAVE_AER
	rc |= __toggle_ras_mc_event(ras, "ras", "aer_event", enable);
#endif

#ifdef HAVE_MCE
	rc |= __toggle_ras_mc_event(ras, "mce", "mce_record", enable);
#endif

free_ras:
	free(ras);
	return rc;
}


/*
 * Tracing read code
 */

static int get_pagesize(struct ras_events *ras, struct pevent *pevent)
{
	int fd, len, page_size = 4096;
	char buf[page_size];

	fd = open_trace(ras, "events/header_page", O_RDONLY);
	if (fd < 0)
		return page_size;

	len = read(fd, buf, page_size);
	if (len <= 0)
		goto error;
	if (pevent_parse_header_page(pevent, buf, len, sizeof(long)))
		goto error;

	page_size = pevent->header_page_data_offset + pevent->header_page_data_size;

error:
	close(fd);
	return page_size;

}

static void parse_ras_data(struct pthread_data *pdata, struct kbuffer *kbuf,
			   void *data, unsigned long long time_stamp)
{
	struct pevent_record record;
	struct trace_seq s;

	record.ts = time_stamp;
	record.size = kbuffer_event_size(kbuf);
	record.data = data;
	record.offset = kbuffer_curr_offset(kbuf);

	/* note offset is just offset in subbuffer */
	record.missed_events = kbuffer_missed_events(kbuf);
	record.record_size = kbuffer_curr_size(kbuf);

	/* TODO - logging */
	trace_seq_init(&s);
	printf("cpu %02d:", pdata->cpu);
	fflush(stdout);
	pevent_print_event(pdata->ras->pevent, &s, &record);
	trace_seq_do_printf(&s);
	printf("\n");
}

static int read_ras_event(int fd,
			  struct pthread_data *pdata,
			  struct kbuffer *kbuf,
			  void *page)
{
	static int warn_sleep = 0;
	unsigned size;
	unsigned long long time_stamp;
	void *data;
#if 0
	/* POLL is currently broken on 3.10-rc2 */
	int ready;
	struct pollfd fds = {
		.fd = fd,
		.events = POLLIN | POLLPRI,
	};
#endif
	do {
#if 0
	/* POLL is currently broken on 3.10-rc2 */
		ready = poll(&fds, 1, -1);
		if (ready < 0) {
			log(TERM, LOG_WARNING, "poll\n");
		}
#endif
		size = read(fd, page, pdata->ras->page_size);
		if (size < 0) {
			log(TERM, LOG_WARNING, "read\n");
			return -1;
		} else if (size > 0) {
			kbuffer_load_subbuffer(kbuf, page);

			while ((data = kbuffer_read_event(kbuf, &time_stamp))) {
				parse_ras_data(pdata, kbuf, data, time_stamp);

				/* increment to read next event */
				kbuffer_next_event(kbuf, NULL);
			}
		} else {
			/*
			 * Before Kernel 3.10, read() never blocks. So, we
			 * need to sleep for a while
			 */
			if (!warn_sleep) {
				log(ALL, LOG_INFO, "Old kernel: need to sleep\n");
				warn_sleep = 1;
			}
			sleep(POLLING_TIME);
		}
	} while (1);
}

static int get_num_cpus(struct ras_events *ras)
{
	char fname[MAX_PATH + 1];
	int num_cpus = 0;
	DIR		*dir;
	struct dirent	*entry;

	strcpy(fname, ras->debugfs);
	strcat(fname, "/tracing/per_cpu/");
	dir = opendir(fname);
	if (!dir)
		return -1;

	for (entry = readdir(dir); entry; entry = readdir(dir)) {
		if (strstr(entry->d_name, "cpu"))
			num_cpus++;
	}
	closedir(dir);

	return num_cpus;
}

static void *handle_ras_events_cpu(void *priv)
{
	int fd;
	struct kbuffer *kbuf;
	void *page;
	char pipe_raw[PATH_MAX];
	struct pthread_data *pdata = priv;

	page = malloc(pdata->ras->page_size);
	if (!page) {
		log(TERM, LOG_ERR, "Can't allocate page\n");
		return NULL;
	}

	kbuf = kbuffer_alloc(KBUFFER_LSIZE_8, ENDIAN);
	if (!kbuf) {
		log(TERM, LOG_ERR, "Can't allocate kbuf");
		free(page);
		return NULL;
	}

	/* FIXME: use select to open for all CPUs */
	snprintf(pipe_raw, sizeof(pipe_raw),
		 "per_cpu/cpu%d/trace_pipe_raw",
		 pdata->cpu);

	fd = open_trace(pdata->ras, pipe_raw, O_RDONLY);
	if (fd < 0) {
		log(TERM, LOG_ERR, "Can't open trace_pipe_raw\n");
		kbuffer_free(kbuf);
		free(page);
		return NULL;
	}

	printf("Listening to events on cpu %d\n", pdata->cpu);
	if (pdata->ras->record_events)
		ras_mc_event_opendb(pdata->cpu, pdata->ras);

	read_ras_event(fd, pdata, kbuf, page);

	close(fd);
	kbuffer_free(kbuf);
	free(page);

	return NULL;
}

#define UPTIME "uptime"

static int select_tracing_timestamp(struct ras_events *ras)
{
	FILE *fp;
	int fd, rc;
	time_t uptime, now;
	unsigned j1;
	char buf[4096];

	/* Check if uptime is supported (kernel 3.10-rc1 or upper) */
	fd = open_trace(ras, "trace_clock", O_RDONLY);
	if (fd < 0) {
		log(TERM, LOG_ERR, "Can't open trace_clock\n");
		return -1;
	}
	read(fd, buf, sizeof(buf));
	close(fd);
	if (!strstr(buf, UPTIME)) {
		log(TERM, LOG_INFO, "Kernel doesn't support uptime clock\n");
		return 0;
	}

	/* Select uptime tracing */
	fd = open_trace(ras, "trace_clock", O_WRONLY);
	if (!fd) {
		log(TERM, LOG_ERR,
		    "Kernel didn't allow writing to trace_clock\n");
		return 0;
	}
	rc = write(fd, UPTIME, sizeof(UPTIME));
	close(fd);

	if (rc < 0) {
		log(TERM, LOG_ERR,
		    "Kernel didn't allow selecting uptime on trace_clock\n");
		return 0;
	}

	/* Reference uptime with localtime */
	fp = fopen("/proc/uptime", "r");
	if (!fp) {
		log(TERM, LOG_ERR,
		    "Couldn't read from /proc/uptime\n");
		return 0;
	}
	fscanf(fp, "%zu.%u ", &uptime, &j1);
	now = time(NULL);
	fclose(fp);

	ras->use_uptime = 1;
	ras->uptime_diff = now - uptime;

	return 0;
}

static int add_event_handler(struct ras_events *ras, struct pevent *pevent,
			     unsigned page_size, char *group, char *event,
			     pevent_event_handler_func func)
{
	int fd, size, rc;
	char *page, fname[MAX_PATH + 1];

	snprintf(fname, sizeof(fname), "events/%s/%s/format", group, event);

	fd = open_trace(ras, fname, O_RDONLY);
	if (fd < 0) {
		log(TERM, LOG_ERR,
		    "Can't get %s:%s format. Are you sure that there's an EDAC driver loaded?\n",
		    group, event);
		return errno;
	}

	page = malloc(page_size);
	if (!page) {
		log(TERM, LOG_ERR, "Can't allocate page to read %s:%s format\n",
		    group, event);
		rc = errno;
		close(fd);
		return rc;
	}

	size = read(fd, page, page_size);
	close(fd);
	if (size < 0) {
		log(TERM, LOG_ERR, "Can't get arch page size\n");
		free(page);
		return size;
	}

	/* Registers the special event handlers */
	rc = pevent_register_event_handler(pevent, -1, group, event, func, ras);
	if (rc == PEVENT_ERRNO__MEM_ALLOC_FAILED) {
		log(TERM, LOG_ERR, "Can't register event handler for %s:%s\n",
		    group, event);
		free(page);
		return EINVAL;
	}

	rc = pevent_parse_event(pevent, page, size, group);
	if (rc) {
		log(TERM, LOG_ERR, "Can't parse event %s:%s\n", group, event);
		free(page);
		return EINVAL;
	}

	/* Enable RAS events */
	rc = __toggle_ras_mc_event(ras, group, event, 1);
	if (rc < 0) {
		log(TERM, LOG_ERR, "Can't enable %s:%s tracing\n",
		    group, event);

		return EINVAL;
	}

	log(ALL, LOG_INFO, "Enabled event %s:%s\n", group, event);

	free(page);
	return 0;
}

int handle_ras_events(int record_events)
{
	int rc, page_size, i, cpus;
	struct pevent *pevent = NULL;
	struct pthread_data *data = NULL;
	struct ras_events *ras = NULL;

	ras = calloc(1, sizeof(*ras));
	if (!ras) {
		log(TERM, LOG_ERR, "Can't allocate memory for ras struct\n");
		return errno;
	}

	rc = get_tracing_dir(ras);
	if (rc < 0) {
		log(TERM, LOG_ERR, "Can't locate a mounted debugfs\n");
		goto err;
	}

	rc = select_tracing_timestamp(ras);
	if (rc < 0) {
		log(TERM, LOG_ERR, "Can't select a timestamp for tracing\n");
		goto err;
	}

	pevent = pevent_alloc();
	if (!pevent) {
		log(TERM, LOG_ERR, "Can't allocate pevent\n");
		rc = errno;
		goto err;
	}

	page_size = get_pagesize(ras, pevent);

	ras->pevent = pevent;
	ras->page_size = page_size;
	ras->record_events = record_events;

	rc = add_event_handler(ras, pevent, page_size, "ras", "mc_event",
			       ras_mc_event_handler);
	if (rc)
		goto err;

#ifdef HAVE_AER
	rc = add_event_handler(ras, pevent, page_size, "ras", "aer_event",
			       ras_aer_event_handler);
	if (rc)
		goto err;
#endif

#ifdef HAVE_MCE
	rc = register_mce_handler(ras);
	if (rc) {
		log(SYSLOG, LOG_INFO, "Can't register mce handler\n");
		goto err;
	}
	if (ras->mce_priv) {
		rc = add_event_handler(ras, pevent, page_size,
				       "mce", "mce_record",
			               ras_mce_event_handler);
		if (rc)
			goto err;
	}
#endif

	cpus = get_num_cpus(ras);
	data = calloc(sizeof(*data), cpus);
	if (!data)
		goto err;

	log(SYSLOG, LOG_INFO, "Opening one thread per cpu (%d threads)\n", cpus);
	for (i = 0; i < cpus; i++) {
		data[i].ras = ras;
		data[i].cpu = i;

		rc = pthread_create(&data[i].thread, NULL,
				    handle_ras_events_cpu,
				    (void *)&data[i]);
		if (rc) {
			log(SYSLOG, LOG_INFO,
			    "Failed to create thread for cpu %d. Aborting.\n",
			    i);
			while (--i)
				pthread_cancel(data[i].thread);
			goto err;
		}
	}

	/* Wait for all threads to complete */
	for (i = 0; i < cpus; i++)
		pthread_join(data[i].thread, NULL);

	log(SYSLOG, LOG_INFO, "Huh! something got wrong. Aborting.\n");

err:
	if (data)
		free(data);

	if (pevent)
		pevent_free(pevent);

	if (ras)
		free(ras);

	return rc;
}
