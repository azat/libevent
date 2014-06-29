/*	$OpenBSD: select.c,v 1.2 2002/06/25 15:50:15 mickey Exp $	*/

/*
 * Copyright 2000-2007 Niels Provos <provos@citi.umich.edu>
 * Copyright 2007-2012 Niels Provos and Nick Mathewson
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "event2/event-config.h"
#include "evconfig-private.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN
#endif
#include <sys/types.h>
#ifdef EVENT__HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include <sys/queue.h>
#ifdef EVENT__HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef EVENT__HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>
#ifdef EVENT__HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef EVENT__HAVE_SIGNALFD
#include <sys/signalfd.h>
#endif

#include "event2/event.h"
#include "event2/event_struct.h"
#include "event-internal.h"
#include "event2/util.h"
#include "evsignal-internal.h"
#include "log-internal.h"
#include "evmap-internal.h"
#include "evthread-internal.h"

/*
  signal.c

  This is the signal-handling implementation we use for backends that don't
  have a better way to do signal handling.  It uses sigaction() or signal()
  or signalfd() to set a signal handler, and a socket pair to tell the event
  base when.

  Note that I said "the event base" : only one event base can be set up to use
  this at a time.  For historical reasons and backward compatibility, if you
  add an event for a signal to event_base A, then add an event for a signal
  (any signal!) to event_base B, event_base B will get informed about the
  signal, but event_base A won't.

  It would be neat to change this behavior in some future version of Libevent.
  kqueue already does something far more sensible.
*/

#ifndef _WIN32
/* Windows wants us to call our signal handlers as __cdecl.  Nobody else
 * expects you to do anything crazy like this. */
#define __cdecl
#endif

static int evsig_add(struct event_base *, evutil_socket_t, short, short, void *);
static int evsig_del(struct event_base *, evutil_socket_t, short, short, void *);
static void evsig_cb(evutil_socket_t fd, short what, void *arg);

static const struct eventop evsigops = {
	"signal",
	NULL,
	evsig_add,
	evsig_del,
	NULL,
	NULL,
	0, 0, 0
};

#ifndef EVENT__DISABLE_THREAD_SUPPORT
/* Lock for evsig_base and evsig_base_n_signals_added fields. */
static void *evsig_base_lock = NULL;
#endif
/* The event base that's currently getting informed about signals. */
static struct event_base *evsig_base = NULL;
/* A copy of evsig_base->sigev_n_signals_added. */
static int evsig_base_n_signals_added = 0;
static evutil_socket_t evsig_base_fd = -1;

/**
 * XXX: Fix this hacky solution
 */
#ifdef EVENT__HAVE_SIGNALFD
static int **sh_old;
static int sh_old_max;

#define sig_prev_info sh_old
#define sig_prev_info_max sh_old_max
#else
#define sig_prev_info base->sig.sh_old
#define sig_prev_info_max base->sig.sh_old_max
#endif

static void __cdecl evsig_handler(int sig);

#define EVSIGBASE_LOCK() EVLOCK_LOCK(evsig_base_lock, 0)
#define EVSIGBASE_UNLOCK() EVLOCK_UNLOCK(evsig_base_lock, 0)

static void
evsig_init_event_(struct event_base *base, int fd)
{
	EVUTIL_ASSERT(fd >= 0);

	event_assign(&base->sig.ev_signal, base, fd,
		EV_READ | EV_PERSIST, evsig_cb, base);

	base->sig.ev_signal.ev_flags |= EVLIST_INTERNAL;
	event_priority_set(&base->sig.ev_signal, 0);
}

void
evsig_set_base_(struct event_base *base)
{
	EVSIGBASE_LOCK();
	evsig_base = base;
	evsig_base_n_signals_added = base->sig.ev_n_signals_added;
#ifdef EVENT__HAVE_SIGNALFD
	evsig_base_fd = base->sig.ev_signal_fd;
#else
	evsig_base_fd = base->sig.ev_signal_pair[1];
#endif
	EVSIGBASE_UNLOCK();
}

/* Callback for when the signal handler write a byte to our signaling socket */
static void
evsig_cb(evutil_socket_t fd, short what, void *arg)
{
#ifdef EVENT__HAVE_SIGNALFD
	struct signalfd_siginfo info;
#else
	static char signals[1024];
#endif
	int i;
	ev_ssize_t n;
	int ncaught[NSIG];
	struct event_base *base;

	base = arg;

	memset(&ncaught, 0, sizeof(ncaught));

#ifdef EVENT__HAVE_SIGNALFD
	while (1) {
		n = read(fd, &info, sizeof(struct signalfd_siginfo));
		if (n != sizeof(struct signalfd_siginfo)) {
			int err = evutil_socket_geterror(fd);
			if (!EVUTIL_ERR_IS_EAGAIN(err))
				event_sock_err(1, fd, "%s: read from sfd", __func__);
			else {
				event_debug(("no more signals at signalfd(%i)", fd));
			}
			break;
		}

		event_debug(("got %s (%i) via signalfd(%i)",
			strsignal(info.ssi_signo), info.ssi_signo, fd));

		ncaught[info.ssi_signo]++;
	}
#else /* EVENT__HAVE_SIGNALFD */
	while (1) {
#ifdef _WIN32
		n = recv(fd, signals, sizeof(signals), 0);
#else
		n = read(fd, signals, sizeof(signals));
#endif
		if (n == -1) {
			int err = evutil_socket_geterror(fd);
			if (! EVUTIL_ERR_RW_RETRIABLE(err))
				event_sock_err(1, fd, "%s: recv", __func__);
			break;
		} else if (n == 0) {
			/* XXX warn? */
			break;
		}
		for (i = 0; i < n; ++i) {
			ev_uint8_t sig = signals[i];
			if (sig < NSIG)
				ncaught[sig]++;
		}
	}
#endif /* EVENT__HAVE_SIGNALFD */

	EVBASE_ACQUIRE_LOCK(base, th_base_lock);
	for (i = 0; i < NSIG; ++i) {
		if (ncaught[i])
			evmap_signal_active_(base, i, ncaught[i]);
	}
	EVBASE_RELEASE_LOCK(base, th_base_lock);
}

int
evsig_init_(struct event_base *base)
{
	int fd;

	/*
	 * Our signal handler is going to write to one end of the socket
	 * pair to wake up our event loop.  The event loop then scans for
	 * signals that got delivered.
	 *
	 * XXX: It will be good if we will fallback to sigaction()/signal()
	 * when signalfd() failed?
	 */
#ifdef EVENT__HAVE_SIGNALFD
	sigset_t mask;
	sigfillset(&mask);

	/* TODO: Add multiple events for one multiple signals */
	fd = signalfd(-1, &mask, SFD_NONBLOCK);
	if (fd == -1) {
		event_warn("signalfd");
		return (-1);
	}

	base->sig.ev_signal_fd = fd;
	evsig_base_fd = fd;
#else
	if (evutil_make_internal_pipe_(base->sig.ev_signal_pair) == -1) {
#ifdef _WIN32
		/* Make this nonfatal on win32, where sometimes people
		   have localhost firewalled. */
		event_sock_warn(-1, "%s: socketpair", __func__);
#else
		event_sock_err(1, -1, "%s: socketpair", __func__);
#endif
		return -1;
	}

	fd = base->sig.ev_signal_pair[0];
#endif /* EVENT__HAVE_SIGNALFD */

	if (sig_prev_info) {
		mm_free(sig_prev_info);
	}
	sig_prev_info = NULL;
	sig_prev_info_max = 0;

	evsig_init_event_(base, fd);

	base->evsigsel = &evsigops;

	return 0;
}

/* Helper: set the signal handler for evsignal to handler in base, so that
 * we can restore the original handler when we clear the current one. */
int
evsig_set_handler_(struct event_base *base,
    int evsignal, void (__cdecl *handler)(int),
    int sa_flags)
{
#if defined(EVENT__HAVE_SIGNALFD)
	sigset_t mask;
	int *s_added;
#elif defined(EVENT__HAVE_SIGACTION)
	struct sigaction sa;
#else
	ev_sighandler_t sh;
#endif
	struct evsig_info *sig = &base->sig;
	void *p;

	/*
	 * resize saved signal handler array up to the highest signal number.
	 * a dynamic array is used to keep footprint on the low side.
	 */
	if (evsignal >= sig_prev_info_max) {
		int new_max = evsignal + 1;
		event_debug(("%s: evsignal (%d) >= sh_old_max (%d), resizing",
			    __func__, evsignal, sig_prev_info_max));
		p = mm_realloc(sig_prev_info, new_max * sizeof(*sig_prev_info));
		if (p == NULL) {
			event_warn("realloc");
			return (-1);
		}

		memset((char *)p + sig_prev_info_max * sizeof(*sig_prev_info),
		    0, (new_max - sig_prev_info_max) * sizeof(*sig_prev_info));

		sig_prev_info_max = new_max;
		sig_prev_info = p;
	}

	/* allocate space for previous handler out of dynamic array */
	if (sig_prev_info[evsignal] == NULL)
		sig_prev_info[evsignal] = mm_calloc(1, sizeof *sig_prev_info[evsignal]);
	if (sig_prev_info[evsignal] == NULL) {
		event_warn("malloc");
		return (-1);
	}

	/* save previous handler and setup new handler */
#if defined(EVENT__HAVE_SIGNALFD)
	sigemptyset(&mask);
	sigaddset(&mask, evsignal);

	if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
		event_warn("sigprocmask for signalfd");
		return (-1);
	}

	s_added = sig_prev_info[evsignal];
	++(*s_added);
#elif defined(EVENT__HAVE_SIGACTION)
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handler;
	sa.sa_flags |= sa_flags;
	sigfillset(&sa.sa_mask);

	if (sigaction(evsignal, &sa, sig->sh_old[evsignal]) == -1) {
		event_warn("sigaction");
		mm_free(sig->sh_old[evsignal]);
		sig->sh_old[evsignal] = NULL;
		return (-1);
	}
#else
	if ((sh = signal(evsignal, handler)) == SIG_ERR) {
		event_warn("signal");
		mm_free(sig->sh_old[evsignal]);
		sig->sh_old[evsignal] = NULL;
		return (-1);
	}
	*sig->sh_old[evsignal] = sh;
#endif

	return (0);
}

static int
evsig_add(struct event_base *base, evutil_socket_t evsignal, short old, short events, void *p)
{
	struct evsig_info *sig = &base->sig;
	int sa_flags = *(int *)p;

	EVUTIL_ASSERT(evsignal >= 0 && evsignal < NSIG);

	/* catch signals if they happen quickly */
	EVSIGBASE_LOCK();
	if (evsig_base != base && evsig_base_n_signals_added) {
		event_warnx("Added a signal to event base %p with signals "
		    "already added to event_base %p.  Only one can have "
		    "signals at a time with the %s backend.  The base with "
		    "the most recently added signal or the most recent "
		    "event_base_loop() call gets preference; do "
		    "not rely on this behavior in future Libevent versions.",
		    base, evsig_base, base->evsel->name);
	}

	/**
	 * XXX: partial copy of evsig_set_base_()
	 */
	evsig_base = base;
	evsig_base_n_signals_added = ++sig->ev_n_signals_added;
#ifdef EVENT__HAVE_SIGNALFD
	evsig_base_fd = base->sig.ev_signal_fd;
#else
	evsig_base_fd = base->sig.ev_signal_pair[1];
#endif
	EVSIGBASE_UNLOCK();

	event_debug(("%s: %d: changing signal handler", __func__, (int)evsignal));
	if (evsig_set_handler_(base, (int)evsignal, evsig_handler, sa_flags) == -1) {
		goto err;
	}

	if (!sig->ev_signal_added) {
		if (event_add_nolock_(&sig->ev_signal, NULL, 0))
			goto err;
		sig->ev_signal_added = 1;
	}

	return (0);

err:
	EVSIGBASE_LOCK();
	--evsig_base_n_signals_added;
	--sig->ev_n_signals_added;
	EVSIGBASE_UNLOCK();
	return (-1);
}

int
evsig_restore_handler_(struct event_base *base, int evsignal)
{
	int ret = 0;
	struct evsig_info *sig = &base->sig;
#if defined(EVENT__HAVE_SIGNALFD)
	int *sh;
	sigset_t mask;
#elif defined(EVENT__HAVE_SIGACTION)
	struct sigaction *sh;
#else
	ev_sighandler_t *sh;
#endif

	if (evsignal >= sig_prev_info_max) {
		/* Can't actually restore. */
		/* XXXX.*/
		return 0;
	}

	/* restore previous handler */
	sh = sig_prev_info[evsignal];

#if defined(EVENT__HAVE_SIGNALFD)
	EVUTIL_ASSERT(*sh > 0);
	--(*sh);

	/* No more events for this signal, can unblock it now. */
	if (!*sh) {
		sigemptyset(&mask);
		sigaddset(&mask, evsignal);

		if (sigprocmask(SIG_UNBLOCK, &mask, NULL) == -1) {
			event_warn("sigprocmask");
			ret = -1;
		}

		sig_prev_info[evsignal] = NULL;
		mm_free(sh);
	}
#elif defined(EVENT__HAVE_SIGACTION)
	if (sigaction(evsignal, sh, NULL) == -1) {
		event_warn("sigaction");
		ret = -1;
	}

	sig->sh_old[evsignal] = NULL;
	mm_free(sh);
#else
	if (signal(evsignal, *sh) == SIG_ERR) {
		event_warn("signal");
		ret = -1;
	}

	sig->sh_old[evsignal] = NULL;
	mm_free(sh);
#endif

	return ret;
}

static int
evsig_del(struct event_base *base, evutil_socket_t evsignal, short old, short events, void *p)
{
	EVUTIL_ASSERT(evsignal >= 0 && evsignal < NSIG);

	event_debug(("%s: "EV_SOCK_FMT": restoring signal handler",
		__func__, EV_SOCK_ARG(evsignal)));

	EVSIGBASE_LOCK();
	--evsig_base_n_signals_added;
	--base->sig.ev_n_signals_added;
	EVSIGBASE_UNLOCK();

	return (evsig_restore_handler_(base, (int)evsignal));
}

#ifdef EVENT__HAVE_SIGNALFD
static void __cdecl
evsig_handler(int sig)
{
}
#else
static void __cdecl
evsig_handler(int sig)
{
	int save_errno = errno;
#ifdef _WIN32
	int socket_errno = EVUTIL_SOCKET_ERROR();
#endif
	ev_uint8_t msg;

	if (evsig_base == NULL) {
		event_warnx(
			"%s: received signal %d, but have no base configured",
			__func__, sig);
		return;
	}

#ifndef EVENT__HAVE_SIGACTION
	signal(sig, evsig_handler);
#endif

	/* Wake up our notification mechanism */
	msg = sig;
#ifdef _WIN32
	send(evsig_base_fd, (char*)&msg, 1, 0);
#else
	{
		int r = write(evsig_base_fd, (char*)&msg, 1);
		(void)r; /* Suppress 'unused return value' and 'unused var' */
	}
#endif
	errno = save_errno;
#ifdef _WIN32
	EVUTIL_SET_SOCKET_ERROR(socket_errno);
#endif
}
#endif /* EVENT__HAVE_SIGNALFD */

void
evsig_dealloc_(struct event_base *base)
{
	int i = 0;
	if (base->sig.ev_signal_added) {
		event_del(&base->sig.ev_signal);
		base->sig.ev_signal_added = 0;
	}
	/* debug event is created in evsig_init_/event_assign even when
	 * ev_signal_added == 0, so unassign is required */
	event_debug_unassign(&base->sig.ev_signal);

	for (i = 0; i < NSIG; ++i) {
		if (i < sig_prev_info_max && sig_prev_info[i] != NULL)
			/* for signalfd() we need loop, since this field is a counter */
			while (sig_prev_info[i] != NULL)
				evsig_restore_handler_(base, i);
	}
	EVSIGBASE_LOCK();
	if (base == evsig_base) {
		evsig_base = NULL;
		evsig_base_n_signals_added = 0;
		evsig_base_fd = -1;
	}
	EVSIGBASE_UNLOCK();

#ifdef EVENT__HAVE_SIGNALFD
	if (base->sig.ev_signal_fd != -1) {
		evutil_closesocket(base->sig.ev_signal_fd);
		base->sig.ev_signal_fd = -1;
	}
#else
	if (base->sig.ev_signal_pair[0] != -1) {
		evutil_closesocket(base->sig.ev_signal_pair[0]);
		base->sig.ev_signal_pair[0] = -1;
	}
	if (base->sig.ev_signal_pair[1] != -1) {
		evutil_closesocket(base->sig.ev_signal_pair[1]);
		base->sig.ev_signal_pair[1] = -1;
	}
#endif
	sig_prev_info_max = 0;

	/* per index frees are handled in evsig_del() */
	if (sig_prev_info) {
		mm_free(sig_prev_info);
		sig_prev_info = NULL;
	}
}

static void
evsig_free_globals_locks(void)
{
#ifndef EVENT__DISABLE_THREAD_SUPPORT
	if (evsig_base_lock != NULL) {
		EVTHREAD_FREE_LOCK(evsig_base_lock, 0);
		evsig_base_lock = NULL;
	}
#endif
	return;
}

void
evsig_free_globals_(void)
{
	evsig_free_globals_locks();
}

#ifndef EVENT__DISABLE_THREAD_SUPPORT
int
evsig_global_setup_locks_(const int enable_locks)
{
	EVTHREAD_SETUP_GLOBAL_LOCK(evsig_base_lock, 0);
	return 0;
}

#endif
