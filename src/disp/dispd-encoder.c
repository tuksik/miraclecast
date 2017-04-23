/*
 * MiracleCast - Wifi-Display/Miracast Implementation
 *
 * Copyright (c) 2013-2014 David Herrmann <dh.herrmann@gmail.com>
 *
 * MiracleCast is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * MiracleCast is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with MiracleCast; If not, see <http://www.gnu.org/licenses/>.
 */
#include <systemd/sd-event.h>
#include <systemd/sd-bus.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <stdarg.h>
#include "dispd-encoder.h"
#include "shl_macro.h"
#include "shl_log.h"
#include "wfd-session.h"
#include "disp.h"
#include "util.h"

struct dispd_encoder
{
	int ref;

	sd_event *loop;
	sd_event_source *child_source;
	sd_event_source *child_term_time_source;
	sd_event_source *pipe_source;

	sd_bus *bus;
	sd_bus_slot *name_disappeared_slot;
	sd_bus_slot *state_change_notify_slot;

	char *name;

	enum dispd_encoder_state state;
	dispd_encoder_state_change_handler handler;
	void *userdata;
};

static int dispd_encoder_new(struct dispd_encoder **out);
static int on_unique_readable(sd_event_source *source,
				int fd,
				uint32_t events,
				void *userdata);
static void dispd_encoder_set_state(struct dispd_encoder *e,
				enum dispd_encoder_state state);

static int dispd_encoder_exec(const char *cmd, int fd, struct wfd_session *s)
{
	int r;
	sigset_t mask;
	char disp[16], auth[256];

	log_info("child forked with pid %d", getpid());

	/* restore to default signal handler */
	sigemptyset(&mask);
	sigprocmask(SIG_SETMASK, &mask, NULL);

	snprintf(disp, sizeof(disp), "DISPLAY=%s", wfd_session_get_disp_name(s));
	snprintf(auth, sizeof(auth), "XAUTHORITY=%s", wfd_session_get_disp_auth(s));

	/* after encoder connected to DBus, write unique name to fd 3,
	 * so we can controll it through DBus
	 */
	r = dup2(fd, 3);
	if(0 > r) {
		return log_ERRNO();
	}

	if(fd != 3) {
		close(fd);
	}

	// TODO run encoder as normal user instead of root
	r = execvpe(cmd,
					(char *[]){ (char *) cmd, NULL },
					(char *[]){ disp,
						auth,
						"G_MESSAGES_DEBUG=all",
						NULL
					});
	_exit(1);
	return 0;
}

static void dispd_encoder_close_pipe(struct dispd_encoder *e)
{
	if(!e->pipe_source) {
		return;
	}

	close(sd_event_source_get_io_fd(e->pipe_source));

	sd_event_source_unref(e->pipe_source);
	e->pipe_source = NULL;

	dispd_encoder_unref(e);
}

static int dispd_encoder_kill_child(struct dispd_encoder *e)
{
	int r;
	pid_t pid;

	if(!e->child_source) {
		return 0;
	}

	// TODO add timer in case child can't be terminated by SIGTERM
	sd_event_source_get_child_pid(e->child_source, &pid);
	r = kill(pid, SIGTERM);
	if(0 > r) {
		return log_ERRNO();
	}

	return 1;
}

static void dispd_encoder_notify_state_change(struct dispd_encoder *e,
				enum dispd_encoder_state state)
{
	assert_vret(e);

	if(!e->handler) {
		return;
	}

	dispd_encoder_ref(e);
	(*e->handler)(e, state, e->userdata);
	dispd_encoder_unref(e);
}

static void dispd_encoder_cleanup(struct dispd_encoder *e)
{
	if(e->child_source) {
		sd_event_source_unref(e->child_source);
		e->child_source = NULL;
		dispd_encoder_unref(e);
	}

	if(e->child_term_time_source) {
		sd_event_source_unref(e->child_term_time_source);
		e->child_term_time_source = NULL;
		dispd_encoder_unref(e);
	}

	if(e->child_source) {
		sd_event_source_unref(e->child_source);
		e->child_source = NULL;
		dispd_encoder_unref(e);
	}

	dispd_encoder_close_pipe(e);

	if(e->name_disappeared_slot) {
		sd_bus_slot_unref(e->name_disappeared_slot);
		e->name_disappeared_slot = NULL;
		dispd_encoder_unref(e);
	}

	if(e->state_change_notify_slot) {
		sd_bus_slot_unref(e->state_change_notify_slot);
		e->state_change_notify_slot = NULL;
		dispd_encoder_unref(e);
	}
}

static int on_child_terminated(sd_event_source *source,
				const siginfo_t *si,
				void *userdata)
{
	struct dispd_encoder *e = userdata;

	log_info("encoder %d terminated", si->si_pid);

	dispd_encoder_set_state(e, DISPD_ENCODER_STATE_TERMINATED);
	dispd_encoder_cleanup(e);

	return 0;
}

int dispd_encoder_spawn(struct dispd_encoder **out, struct wfd_session *s)
{
	pid_t pid;
	_dispd_encoder_unref_ struct dispd_encoder *e = NULL;
	int fds[2] = { -1, -1 };
	int r;

	assert_ret(out);
	assert_ret(s);

	r = dispd_encoder_new(&e);
	if(0 > r) {
		goto end;
	}

	r = pipe(fds);
	if(0 > r) {
		goto end;
	}

	pid = fork();
	if(0 > pid) {
		r = pid;
		goto kill_encoder;
	}
	else if(!pid) {
		close(fds[0]);

		r = dispd_encoder_exec("gstencoder", fds[1], s);
		if(0 > r) {
			log_warning("failed to exec encoder: %s", strerror(errno));
		}
		_exit(1);
	}

	r = sd_event_add_child(ctl_wfd_get_loop(),
					&e->child_source,
					pid,
					WEXITED,
					on_child_terminated,
					dispd_encoder_ref(e));
	if(0 > r) {
		goto close_pipe;
	}

	r = sd_event_add_io(ctl_wfd_get_loop(),
					&e->pipe_source,
					fds[0],
					EPOLLIN,
					on_unique_readable,
					dispd_encoder_ref(e));
	if(0 > r) {
		goto close_pipe;
	}

	close(fds[1]);
	*out = dispd_encoder_ref(e);

	return 0;

close_pipe:
	close(fds[0]);
	close(fds[1]);
kill_encoder:
	// dispd will do the cleanup
	kill(pid, SIGKILL);
end:
	return log_ERRNO();
}

static int dispd_encoder_new(struct dispd_encoder **out)
{
	_shl_free_ struct dispd_encoder *e = NULL;

	assert_ret(out);
   
	e = calloc(1, sizeof(struct dispd_encoder));
	if(!e) {
		return log_ENOMEM();
	}

	e->ref = 1;
	*out = e;
	e = NULL;

	return 0;
}

struct dispd_encoder * dispd_encoder_ref(struct dispd_encoder *e)
{
	assert_retv(e, NULL);
	assert_retv(0 < e->ref, NULL);

	++ e->ref;

	return e;
}

void dispd_encoder_unrefp(struct dispd_encoder **e)
{
	if(*e) {
		dispd_encoder_unref(*e);
	}
}

void dispd_encoder_unref(struct dispd_encoder *e)
{
	assert_vret(e);
	assert_vret(0 < e->ref);

	--e->ref;
	if(e->ref) {
		return;
	}

	/* since we encrease ref count at creation of every sources and slots,
	 * once we get here, it means no sources and slots exist anymore */
	if(e->bus) {
		sd_bus_unref(e->bus);
	}

	if(e->name) {
		free(e->name);
	}

	free(e);
}

void dispd_encoder_set_handler(struct dispd_encoder *e,
				dispd_encoder_state_change_handler handler,
				void *userdata)
{
	assert_vret(e);

	e->handler = handler;
	e->userdata = userdata;
}

dispd_encoder_state_change_handler dispd_encoder_get_handler(struct dispd_encoder *e)
{
	assert_retv(e, NULL);

	return e->handler;
}

enum dispd_encoder_state dispd_encoder_get_state(struct dispd_encoder *e)
{
	assert_retv(e, DISPD_ENCODER_STATE_NULL);
	
	return e->state;
}

static const char * state_to_name(enum dispd_encoder_state s)
{
	const char *names[] = {
		"NULL",
		"SPAWNED",
		"CONFIGURED",
		"READY",
		"STARTED",
		"PAUSED",
		"TERMINATED"
	};

	if(0 > s || DISPD_ENCODER_STATE_TERMINATED < s) {
		return "unknown encoder state";
	}

	return names[s];
}

static void dispd_encoder_set_state(struct dispd_encoder *e,
				enum dispd_encoder_state state)
{
	assert_vret(e);
	
	if(e->state == state) {
		return;
	}

	log_debug("state change from %s to %s",
					state_to_name(e->state),
					state_to_name(state));

	e->state = state;
	dispd_encoder_notify_state_change(e, state);
}

static int on_encoder_properties_changed(sd_bus_message *m,
				void *userdata,
				sd_bus_error *ret_error)
{
	struct dispd_encoder *e = userdata;
	const char *name;
	int value;
	enum dispd_encoder_state s;
	int r;

	r = sd_bus_message_skip(m, "s");
	if(0 > r) {
		return log_ERRNO();
	}

	r = sd_bus_message_enter_container(m, 'a', "{sv}");
	if(0 > r) {
		return log_ERRNO();
	}

	while(!sd_bus_message_at_end(m, true)) {
		r = sd_bus_message_read(m, "{sv}", &name, "i", &value);
		if(0 > r) {
			return log_ERRNO();
		}

		if(strcmp("State", name)) {
			continue;
		}

		switch(value) {
			case 0:
				s = DISPD_ENCODER_STATE_NULL;
				break;
			case 1:
				s = DISPD_ENCODER_STATE_CONFIGURED;
				break;
			case 2:
				s = DISPD_ENCODER_STATE_READY;
				break;
			case 3:
				s = DISPD_ENCODER_STATE_STARTED;
				break;
			case 4:
				s = DISPD_ENCODER_STATE_PAUSED;
				break;
			case 5:
				s = DISPD_ENCODER_STATE_TERMINATED;
				break;
			default:
				log_error("encoder enter unknown state: %d", value);
				return 0;
		}

		dispd_encoder_set_state(e, s);
		break;
	}

	return 0;
}

static int on_encoder_disappeared(sd_bus_message *m,
				void *userdata,
				sd_bus_error *ret_error)
{
	struct dispd_encoder *e = userdata;
	int r;

	log_info("encoder %s disappered", e->name);

	r = dispd_encoder_kill_child(e);
	if(0 > r) {
		return log_ERRNO();
	}
	else if(r) {
		return 0;
	}

	dispd_encoder_cleanup(e);

	return 0;
}

static int on_unique_readable(sd_event_source *source,
				int fd,
				uint32_t events,
				void *userdata)
{
	struct dispd_encoder *e = userdata;
	char buf[1024];
	ssize_t r;

	r = read(fd, buf, sizeof(buf) - 1);
	if(0 > r) {
		if(EAGAIN == errno) {
			return 0;
		}

		goto error;
	}
	else if(!r) {
		log_warning("no bus name returned from encoder: %s",
						strerror(errno));
		goto error;
	}

	// TODO remove heading and trailing speces from buf before strdup()
	buf[r] = '\0';
	log_info("got bus name from encoder: %s", buf);

	e->name = strdup(buf);
	if(!e->name) {
		goto error;
	}

	// TODO connect to encoder through user session bus
	r = sd_bus_default_system(&e->bus);
	if(0 > r) {
		goto error;
	}

	snprintf(buf, sizeof(buf), 
					"type='signal',"
						"sender='%s',"
							"path='/org/freedesktop/miracle/encoder',"
						"interface='org.freedesktop.DBus.Properties',"
							"member='PropertiesChanged',"
							"arg0='org.freedesktop.miracle.encoder'",
					e->name);
	r = sd_bus_add_match(e->bus,
						&e->state_change_notify_slot,
						buf,
						on_encoder_properties_changed,
						dispd_encoder_ref(e));
	if(0 > r) {
		goto error;
	}

	snprintf(buf, sizeof(buf), 
					"type='signal',"
						"sender='org.freedesktop.DBus',"
							"path='/org/freedesktop/DBus',"
						"interface='org.freedesktop.DBus',"
							"member='NameOwnerChanged',"
							"arg0namespace='%s'",
					e->name);
	r = sd_bus_add_match(e->bus,
						&e->name_disappeared_slot,
						buf,
						on_encoder_disappeared,
						dispd_encoder_ref(e));

	dispd_encoder_set_state(e, DISPD_ENCODER_STATE_SPAWNED);

	goto end;

error:
	log_vERRNO();
	dispd_encoder_kill_child(e);
end:
	dispd_encoder_close_pipe(e);

	return r;
}

static int config_append(sd_bus_message *m,
				enum wfd_encoder_config k,
				const char *t,
				...)
{
	int r;
	va_list argv;

	assert_ret(m);
	assert_ret(t);

	r = sd_bus_message_open_container(m, 'e', "iv");
	if(0 > r) {
		return log_ERRNO();
	}

	r = sd_bus_message_append(m, "i", k);
	if(0 > r) {
		return log_ERRNO();
	}

	r = sd_bus_message_open_container(m, 'v', t);
	if(0 > r) {
		return log_ERRNO();
	}

	va_start(argv, t);
	switch(*t) {
		case 's':
			r = sd_bus_message_append(m, t, va_arg(argv, char *));
			break;
		case 'u':
			r = sd_bus_message_append(m, t, va_arg(argv, uint32_t));
			break;
		default:
			abort();
	}
	va_end(argv);

	if(0 > r) {
		return log_ERRNO();
	}

	r = sd_bus_message_close_container(m);
	if(0 > r) {
		return log_ERRNO();
	}

	r = sd_bus_message_close_container(m);
	if(0 > r) {
		return log_ERRNO();
	}

	return 0;
}

int dispd_encoder_configure(struct dispd_encoder *e, struct wfd_session *s)
{
	_cleanup_sd_bus_message_ sd_bus_message *call = NULL;
	_cleanup_sd_bus_message_ sd_bus_message *reply = NULL;
	_cleanup_sd_bus_error_ sd_bus_error error = SD_BUS_ERROR_NULL;
	const struct wfd_rectangle *rect;
	struct wfd_sink *sink;
	int r;

	assert_ret(e);
	assert_ret(e->bus);
	assert_ret(s);
	assert_ret(wfd_is_out_session(s));

	r = sd_bus_message_new_method_call(e->bus,
					&call,
					e->name,
					"/org/freedesktop/miracle/encoder",
					"org.freedesktop.miracle.encoder",
					"Configure");
	if(0 > r) {
		return log_ERRNO();
	}

	r = sd_bus_message_open_container(call, 'a', "{iv}");
	if(0 > r) {
		return log_ERRNO();
	}

	sink = wfd_out_session_get_sink(s);
	r = config_append(call,
					WFD_ENCODER_CONFIG_PEER_ADDRESS,
					"s",
					sink->peer->remote_address);
	if(0 > r) {
		return log_ERRNO();
	}

	r = config_append(call,
					WFD_ENCODER_CONFIG_RTP_PORT0,
					"u",
					s->stream.rtp_port);
	if(0 > r) {
		return log_ERRNO();
	}

	if(s->stream.rtcp_port) {
		r = config_append(call,
						WFD_ENCODER_CONFIG_PEER_RTCP_PORT,
						"u",
						s->stream.rtcp_port);
		if(0 > r) {
			return log_ERRNO();
		}
	}

	r = config_append(call,
					WFD_ENCODER_CONFIG_LOCAL_ADDRESS,
					"s",
					sink->peer->local_address);
	if(0 > r) {
		return log_ERRNO();
	}

	if(s->stream.rtcp_port) {
		r = config_append(call,
						WFD_ENCODER_CONFIG_LOCAL_RTCP_PORT,
						"u",
						s->stream.rtcp_port);
		if(0 > r) {
			return log_ERRNO();
		}
	}

	rect = wfd_session_get_disp_dimension(s);
	if(rect) {
		r = config_append(call,
						WFD_ENCODER_CONFIG_X,
						"u",
						rect->x);
		if(0 > r) {
			return log_ERRNO();
		}

		r = config_append(call,
						WFD_ENCODER_CONFIG_Y,
						"u",
						rect->y);
		if(0 > r) {
			return log_ERRNO();
		}

		r = config_append(call,
						WFD_ENCODER_CONFIG_WIDTH,
						"u",
						rect->width);
		if(0 > r) {
			return log_ERRNO();
		}

		r = config_append(call,
						WFD_ENCODER_CONFIG_HEIGHT,
						"u",
						rect->height);
		if(0 > r) {
			return log_ERRNO();
		}
	}

	r = sd_bus_message_close_container(call);
	if(0 > r) {
		return log_ERRNO();
	}

	r = sd_bus_call(e->bus, call, 0, &error, &reply);
	if(0 > r) {
		log_warning("%s: %s", error.name, error.message);
		sd_bus_error_free(&error);
		return log_ERRNO();
	}

	return 0;
}

static int dispd_encoder_call(struct dispd_encoder *e, const char *method)
{
	_cleanup_sd_bus_message_ sd_bus_message *call = NULL;
	_cleanup_sd_bus_message_ sd_bus_message *reply = NULL;
	_cleanup_sd_bus_error_ sd_bus_error error = SD_BUS_ERROR_NULL;
	int r;

	assert_ret(e);
	assert_ret(method);
	assert_ret(e->bus);

	r = sd_bus_message_new_method_call(e->bus,
					&call,
					e->name,
					"/org/freedesktop/miracle/encoder",
					"org.freedesktop.miracle.encoder",
					method);
	if(0 > r) {
		log_vERR(r);
		goto error;
	}

	r = sd_bus_call(e->bus, call, 0, &error, &reply);
	if(0 > r) {
		log_warning("%s: %s", error.name, error.message);
		goto error;
	}

	return 0;

error:
	dispd_encoder_kill_child(e);

	return r;
}

int dispd_encoder_start(struct dispd_encoder *e)
{
	assert_ret(e);

	return dispd_encoder_call(e, "Start");
}

int dispd_encoder_pause(struct dispd_encoder *e)
{
	assert_ret(e);

	return dispd_encoder_call(e, "Pause");
}

static int on_child_term_timeout(sd_event_source *s,
				uint64_t usec,
				void *userdata)
{
	struct dispd_encoder *e = userdata;

	dispd_encoder_kill_child(e);

	return 0;
}

int dispd_encoder_stop(struct dispd_encoder *e)
{
	uint64_t now;
	sd_event *loop;
	int r;

	assert_ret(e);

	r = dispd_encoder_call(e, "Stop");
	if(0 > r) {
		return r;
	}

	loop = ctl_wfd_get_loop();
	r = sd_event_now(loop, CLOCK_MONOTONIC, &now);
	if(0 > r) {
		log_vERR(r);
		goto error;
	}

	r = sd_event_add_time(loop,
					&e->child_term_time_source,
					CLOCK_MONOTONIC,
					now + (1000 * 1000),
					0,
					on_child_term_timeout,
					dispd_encoder_ref(e));
	if(0 > r) {
		log_vERR(r);
		goto error;
	}

error:
	dispd_encoder_kill_child(e);
	return r;
}
