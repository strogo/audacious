/*
 * audacious: Cross-platform multimedia player.
 * signals.c: Signal handling.
 *
 * Copyright (c) 2005-2007 Audacious development team.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

//#define _XOPEN_SOURCE
#include <unistd.h>	/* for signal_check_for_broken_impl() */

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gprintf.h>
#include <config.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <pthread.h>	/* for pthread_sigmask() */
#include <signal.h>
#include <strings.h>

#ifdef HAVE_EXECINFO_H
# include <execinfo.h>
#endif

#include "main.h"
#include "ui_main.h"
#include "signals.h"
#include "build_stamp.h"

gint linuxthread_signal_number = 0;

static void
signal_process_segv(void)
{
    g_printerr(_("\nAudacious has caught signal 11 (SIGSEGV).\n\n"
         "We apologize for the inconvenience, but Audacious has crashed.\n"
         "This is a bug in the program, and should never happen under normal circumstances.\n"
	 "Your current configuration has been saved and should not be damaged.\n\n"
	 "You can help improve the quality of Audacious by filing a bug at http://bugs-meta.atheme.org\n"
         "Please include the entire text of this message and a description of what you were doing when\n"
         "this crash occured in order to quickly expedite the handling of your bug report:\n\n"));

    g_printerr("Program version: Audacious %s (buildid: %s)\n\n", VERSION, svn_stamp);

#ifdef HAVE_EXECINFO_H
    {
        void *stack[20];
        size_t size;
        char **strings;
        size_t i;

        size = backtrace(stack, 20);
        strings = backtrace_symbols(stack, size);

        g_printerr("Stacktrace (%zd frames):\n", size);

        for (i = 0; i < size; i++)
            g_printerr("   %ld. %s\n", (long)i + 1, strings[i]);

        g_free(strings);
    }
#else
    g_printerr("Stacktrace was unavailable.\n");
#endif

    g_printerr(_("\nBugs can be reported at http://bugs-meta.atheme.org against the Audacious product.\n"));

    g_critical("Received SIGSEGV -- Audacious has crashed.");

    bmp_config_save();
    abort();
}

static void *
signal_process_signals (void *data)
{
    sigset_t waitset;
    int sig;

    sigemptyset(&waitset);
    sigaddset(&waitset, SIGPIPE);
    sigaddset(&waitset, SIGSEGV);  
    sigaddset(&waitset, SIGINT);
    sigaddset(&waitset, SIGTERM);

    while(1) {
        sigwait(&waitset, &sig);

        switch(sig){
        case SIGPIPE:
            /*
             * do something.
             */
            break;

        case SIGSEGV:
            signal_process_segv();
            break;

        case SIGINT:
            g_print("Audacious has received SIGINT and is shutting down.\n");
            mainwin_quit_cb();
            break;

        case SIGTERM:
            g_print("Audacious has received SIGTERM and is shutting down.\n");
            mainwin_quit_cb();
            break;
        }
    }

    return NULL; //dummy
}

/********************************************************************************/
/* for linuxthread */
/********************************************************************************/

typedef void (*SignalHandler) (gint);

static void *
signal_process_signals_linuxthread (void *data)
{
    while(1) {
        g_usleep(1000000);

        switch(linuxthread_signal_number){
        case SIGPIPE:
            /*
             * do something.
             */
            linuxthread_signal_number = 0;
            break;

        case SIGSEGV:
            signal_process_segv();
            break;

        case SIGINT:
            g_print("Audacious has received SIGINT and is shutting down.\n");
            mainwin_quit_cb();
            break;

        case SIGTERM:
            g_print("Audacious has received SIGTERM and is shutting down.\n");
            mainwin_quit_cb();
            break;
        }
    }

    return NULL; //dummy
}

static void
linuxthread_handler (gint signal_number)
{
    /* note: cannot manipulate mutex from signal handler */
    linuxthread_signal_number = signal_number;
}

static SignalHandler
signal_install_handler_full (gint           signal_number,
                             SignalHandler  handler,
                             gint          *signals_to_block,
                             gsize          n_signals)
{
    struct sigaction action, old_action;
    gsize i;

    action.sa_handler = handler;
    action.sa_flags = SA_RESTART;

    sigemptyset (&action.sa_mask);

    for (i = 0; i < n_signals; i++)
        sigaddset (&action.sa_mask, signals_to_block[i]);

    if (sigaction (signal_number, &action, &old_action) == -1)
    {
        g_message ("Failed to install handler for signal %d", signal_number);
        return NULL;
    }

    return old_action.sa_handler;
}

/* 
 * A version of signal() that works more reliably across different
 * platforms. It: 
 * a. restarts interrupted system calls
 * b. does not reset the handler
 * c. blocks the same signal within the handler
 *
 * (adapted from Unix Network Programming Vol. 1)
 */
static SignalHandler
signal_install_handler (gint          signal_number,
                        SignalHandler handler)
{
    return signal_install_handler_full (signal_number, handler, NULL, 0);
}


/* sets up blocking signals for pthreads. 
 * linuxthreads sucks and needs this to make sigwait(2) work 
 * correctly. --nenolod
 *
 * correction -- this trick does not work on linuxthreads.
 * going to keep it in it's own function though --nenolod
 */
static void
signal_initialize_blockers(void)
{
    sigset_t blockset;

    sigemptyset(&blockset);
    sigaddset(&blockset, SIGPIPE);
    sigaddset(&blockset, SIGSEGV);  
    sigaddset(&blockset, SIGINT);
    sigaddset(&blockset, SIGTERM);

    if(pthread_sigmask(SIG_BLOCK, &blockset, NULL))
        g_print("pthread_sigmask() failed.\n");    
}

static gboolean
signal_check_for_broken_impl(void)
{
#ifdef _CS_GNU_LIBPTHREAD_VERSION
    {
        gchar str[1024];
        confstr(_CS_GNU_LIBPTHREAD_VERSION, str, sizeof(str));

        if (!strncasecmp("linuxthreads", str, 12))
            return TRUE;
    }
#endif

    return FALSE;
}

void 
signal_handlers_init(void)
{
    if (signal_check_for_broken_impl() != TRUE)
    {
        signal_initialize_blockers();
        g_thread_create(signal_process_signals, NULL, FALSE, NULL);
    }
    else
    {
        g_printerr(_("Your signaling implementation is broken.\n"
		     "Expect unusable crash reports.\n"));

        /* install special handler which catches signals and forwards to the signal handling thread */
        signal_install_handler(SIGPIPE, linuxthread_handler);
        signal_install_handler(SIGSEGV, linuxthread_handler);
        signal_install_handler(SIGINT, linuxthread_handler);
        signal_install_handler(SIGTERM, linuxthread_handler);

        /* create handler thread */
        g_thread_create(signal_process_signals_linuxthread, NULL, FALSE, NULL);

    }
}
