/*
 * Copyright (c) 1980 Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * 1999-02-22 Arkadiusz Mi¶kiewicz <misiek@pld.ORG.PL>
 * - added Native Language Support
 *
 * 2000-07-30 Per Andreas Buer <per@linpro.no> - added "q"-option
 */

/*
 * script
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <fcntl.h>
#include <locale.h>
#include <stropts.h>
#include <sysexits.h>

#define _(Text) (Text)

// Work around bad NULL definition *somewhere* in our headers
#ifdef NULL
# undef NULL
#endif
#define NULL ((void*)0)

#ifndef _PATH_BSHELL
# define _PATH_BSHELL "/home/gschijnd/usr/bin/zsh"
#endif

#define BUFSIZE (65536UL)

static void finish(int);
static void fail(void) __attribute__((__noreturn__));
static void resize(int);
static void fixtty(const struct termios*);
static void getmaster(void);
static int getslave(const char* pts, const struct termios* origtty);
static int doio(const struct termios* origtty, const int pty);
static int doshell(const char* pts, const struct termios* origtty);

static int master = -1, resizein = -1, resizeout = -1;
static pid_t child;
static int childstatus;
static const char* fname;

static int aflg = 0;
static const char* cflg = NULL;
static int eflg = 0;
static int fflg = 0;
static int qflg = 0;
static int tflg = 0;

static const char* progname;

static volatile bool die;

static void
die_if_link(const char* fn) {
	struct stat s;

	if (lstat(fn, &s) == 0 && (S_ISLNK(s.st_mode) || s.st_nlink > 1)) {
		fprintf(stderr,
			_("Warning: `%s' is a link.\n"
			  "Use `%s [options] %s' if you really "
			  "want to use it.\n"
			  "Script not started.\n"),
			fn, progname, fn);
		exit(1);
	}
}

/*
 * script -t prints time delays as floating point numbers
 * The example program (scriptreplay) that we provide to handle this
 * timing output is a perl script, and does not handle numbers in
 * locale format (not even when "use locale;" is added).
 * So, since these numbers are not for human consumption, it seems
 * easiest to set LC_NUMERIC here.
 */

int
main(int argc, char **argv) {
	sigset_t block_mask, unblock_mask;
	extern int optind;
	const char* p;
	int ch;

	progname = argv[0];
	if ((p = strrchr(progname, '/')) != NULL)
		progname = p+1;


	setlocale(LC_ALL, "");
	setlocale(LC_NUMERIC, "C");	/* see comment above */

	if (argc == 2) {
		if (!strcmp(argv[1], "-V") || !strcmp(argv[1], "--version")) {
			printf(_("%s (giel-customized-script 0.1)\n"),
			       progname/* , PACKAGE_STRING */);
			return 0;
		}
	}

	while ((ch = getopt(argc, argv, "ac:efqt")) != -1)
		switch((char)ch) {
		case 'a':
			aflg++;
			break;
		case 'c':
			cflg = optarg;
			break;
		case 'e':
			eflg++;
			break;
		case 'f':
			fflg++;
			break;
		case 'q':
			qflg++;
			break;
		case 't':
			tflg++;
			break;
		case '?':
		default:
			fprintf(stderr,
				_("usage: script [-a] [-e] [-f] [-q] [-t] [file]\n"));
			return EX_USAGE;
		}
	argc -= optind;
	argv += optind;

	if (argc > 0)
		fname = argv[0];
	else {
		fname = "typescript";
		die_if_link(fname);
	}

	getmaster();
	if (!qflg)
		printf(_("Script started, file is %s\n"), fname);
	const char* pts = ptsname(master);
	if (!pts) {
		perror("ptsname");
		fail();
	}

	struct termios origtty;
	tcgetattr(STDIN_FILENO, &origtty);

	/* init mask for SIGCHLD */
	sigprocmask(SIG_SETMASK, NULL, &block_mask);
	sigaddset(&block_mask, SIGCHLD);

	sigprocmask(SIG_SETMASK, &block_mask, &unblock_mask);
	child = fork();
	sigprocmask(SIG_SETMASK, &unblock_mask, NULL);

	if (child == -1) {
		perror("fork");
		fail();
	}
	if (child == 0) {
		close(master);
		close(resizein);
		close(resizeout);
		master = resizein = resizeout = -1;
		return doshell(pts, &origtty);
	}

	/* setup SIGCHLD handler */
	struct sigaction sa;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sa.sa_handler = finish;
	sigaction(SIGCHLD, &sa, NULL);

	/* SIGWINCH handler */
	sa.sa_handler = resize;
	sigaction(SIGWINCH, &sa, NULL);

	return doio(&origtty, master);
}

#define MAX(a,b) ((a) < (b) ? (b) : (a))
#define MIN(a,b) ((a) < (b) ? (a) : (b))

static int
doio(const struct termios* origtty, const int pty) {
	bool stdin_open  = true,
	     stdout_open = true,
	     script_open = true,
	     ptyin_open  = true,
	     ptyout_open = true;
	char ptyoutbuf[BUFSIZE],
	     stdoutbuf[BUFSIZE],
	     scriptbuf[BUFSIZE];
	size_t ptyoutpending = 0,
	       stdoutpending = 0,
	       scriptpending = 0;
	static const size_t delay_spec_size  = sizeof("\x1B_D;18446744073709551615.999999\x1B\\") - 1;
	static const size_t resize_spec_size = sizeof("\x1B[8;65535;65535t") - 1;

	const int scriptfd = open(fname, O_WRONLY | O_CREAT | (aflg ? O_APPEND : O_TRUNC)
			/* Flush data after each write when requested. */
#if O_DSYNC
			| (fflg ? O_DSYNC : 0)
#elif O_SYNC
			| (fflg ? O_SYNC : 0)
#elif O_FSYNC
			| (fflg ? O_FSYNC : 0)
#endif
			, 0666);
	if (scriptfd == -1) {
		perror(fname);
		fail();
	}

	struct timeval oldtime, newtime;
	gettimeofday(&newtime, NULL);
	oldtime = newtime;
	{
		char tbuf[256];
		if (strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S %Z\n", gmtime(&newtime.tv_sec)))
			scriptpending = snprintf(scriptbuf, sizeof(scriptbuf), _("Script started on %s\r\n"), tbuf);
		else
			scriptpending = snprintf(scriptbuf, sizeof(scriptbuf), "%s", _("Script started\r\n"));
	}

	const int nfds = MAX(STDIN_FILENO, MAX(STDOUT_FILENO, MAX(pty, scriptfd))) + 1;

	fixtty(origtty);
	int exitcode = EX_OK;

	while (stdin_open || (ptyout_open && ptyoutpending)
	    || ptyin_open || (stdout_open && stdoutpending) || (script_open && scriptpending))
	{
		fd_set rfds, wfds;
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);

		if (stdin_open && ptyoutpending < sizeof(ptyoutbuf))
			FD_SET(STDIN_FILENO, &rfds);
		if (stdout_open && stdoutpending)
			FD_SET(STDOUT_FILENO, &wfds);
		if (script_open && scriptpending)
			FD_SET(scriptfd, &wfds);

		if (ptyin_open && MAX(stdoutpending, scriptpending + delay_spec_size) < MIN(sizeof(stdoutbuf), sizeof(scriptbuf)))
			FD_SET(pty, &rfds);
		if (ptyout_open && ptyoutpending)
			FD_SET(pty, &wfds);

		if (resizein != -1)
			FD_SET(resizein, &rfds);

		const int ret = select(nfds, &rfds, &wfds, NULL, NULL);
		if (ret == -1)
		{
			if (errno == EINTR)
				continue;
			perror("select");
			exitcode = EX_OSERR;
			goto restoretty;
		}

		gettimeofday(&newtime, NULL);

		// Send data down the pseudo terminal first
		if (ptyoutpending && FD_ISSET(pty, &wfds))
		{
			ssize_t ret = write(pty, ptyoutbuf, ptyoutpending);
			if (ret == -1)
			{
				switch (errno)
				{
					case EINTR:
						break;
					case ECONNRESET:
					case EPIPE:
						ptyout_open = false;
						break;
					default:
						perror("write(pty)");
						exitcode = EX_IOERR;
						goto restoretty;
				}
			}
			else
			{
				ptyoutpending -= ret;
				memmove(ptyoutbuf, ptyoutbuf + ret, ptyoutpending);
			}
		}

		// Send data down stdout next
		if (stdoutpending && FD_ISSET(STDOUT_FILENO, &wfds))
		{
			ssize_t ret = write(STDOUT_FILENO, stdoutbuf, stdoutpending);
			if (ret == -1)
			{
				switch (errno)
				{
					case EINTR:
						break;
					case ECONNRESET:
					case EPIPE:
						close(STDOUT_FILENO);
						stdout_open = false;
						break;
					default:
						perror("write");
						exitcode = EX_IOERR;
						goto restoretty;
				}
			}
			else
			{
				stdoutpending -= ret;
				memmove(stdoutbuf, stdoutbuf + ret, stdoutpending);
			}
		}

		// Send data down typescript next
		if (scriptpending && FD_ISSET(scriptfd, &wfds))
		{
			ssize_t ret = write(scriptfd, scriptbuf, scriptpending);
			if (ret == -1)
			{
				switch (errno)
				{
					case EINTR:
						break;
					case ECONNRESET:
					case EPIPE:
						close(scriptfd);
						script_open = false;
						break;
					default:
						perror("write");
						exitcode = EX_IOERR;
						goto restoretty;
				}
			}
			else
			{
				scriptpending -= ret;
				memmove(scriptbuf, scriptbuf + ret, scriptpending);
			}

			if (!scriptpending && !ptyin_open && !qflg)
			{
				char tbuf[256];
				if (strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S %Z\n", gmtime(&newtime.tv_sec)))
					scriptpending = snprintf(scriptbuf, sizeof(scriptbuf), _("\r\nScript done on %s\r\n"), tbuf);
				else
					scriptpending = snprintf(scriptbuf, sizeof(scriptbuf), "%s", _("\r\nScript done\r\n"));
			}
		}

		// Process resizes ASAP
		if (scriptpending + resize_spec_size < sizeof(scriptbuf) && FD_ISSET(resizein, &rfds))
		{
			struct winsize win;
			ssize_t ret = read(resizein, &win, sizeof(win));
			if (ret == -1)
			{
				switch (errno)
				{
					case EINTR:
						break;
					default:
						perror("read(resizepipe)");
						exitcode = EX_IOERR;
						goto restoretty;
				}
			}
			else if (ret == 0)
			{
				close(resizeout);
				close(resizein);
				resizein = resizeout = -1;
			}
			else
			{
				const int len = snprintf(scriptbuf + scriptpending, sizeof(scriptbuf) - scriptpending,
						"\x1B[8;%hu;%hut", win.ws_row, win.ws_col);
				if (len >= 0 && len < sizeof(scriptbuf) - scriptpending)
					scriptpending += len;
			}
		}

		// Fetch data from the pseudo terminal first
		if (MAX(stdoutpending, scriptpending + delay_spec_size) < MIN(sizeof(stdoutbuf), sizeof(scriptbuf)) && FD_ISSET(pty, &rfds))
		{
			ssize_t ret = read(pty, stdoutbuf + stdoutpending, MIN(sizeof(stdoutbuf), sizeof(scriptbuf)) - MAX(stdoutpending, scriptpending + delay_spec_size));
			if (ret == -1)
			{
				switch (errno)
				{
					case EIO:
						ptyin_open = false;
						break;
					case EINTR:
						break;
					default:
						perror("read(pty)");
						exitcode = EX_IOERR;
						goto restoretty;
				}
			}
			else if (ret == 0)
			{
				ptyin_open = false;
			}
			else
			{
				const int usec_compensation = (newtime.tv_usec > oldtime.tv_usec) ? 0 : 1;
				const struct timeval diff = {
					.tv_sec  = newtime.tv_sec  - oldtime.tv_sec  - usec_compensation,
					.tv_usec = newtime.tv_usec - oldtime.tv_usec + usec_compensation * 1000000L,
				};
				oldtime = newtime;

				// Use Application Program-Control code to add delay-command scriptreplay can use
				const int len = snprintf(scriptbuf + scriptpending, sizeof(scriptbuf) - scriptpending,
						"\x1B_D;%lld.%06ld\x1B\\", (long long)diff.tv_sec, (long)diff.tv_usec);
				if (len >= 0 && len < sizeof(scriptbuf) - scriptpending)
					scriptpending += len;

				if (tflg) {
					fprintf(stderr, "%03lld.%06ld %zd\n", (long long)diff.tv_sec, (long)diff.tv_usec, ret);
				}

				// Make sure the data is available in the scriptbuf as well
				memcpy(scriptbuf + scriptpending, stdoutbuf + stdoutpending, ret);
				stdoutpending += ret;
				scriptpending += ret;
			}
		}

		// Fetch data from stdin next
		if (ptyoutpending < sizeof(ptyoutbuf) && FD_ISSET(STDIN_FILENO, &rfds))
		{
			ssize_t ret = read(STDIN_FILENO, ptyoutbuf + ptyoutpending, sizeof(ptyoutbuf) - ptyoutpending);
			if (ret == -1)
			{
				switch (errno)
				{
					case EINTR:
						break;
					default:
						perror("read");
						exitcode = EX_IOERR;
						goto restoretty;
				}
			}
			else if (ret == 0)
			{
				close(STDIN_FILENO);
				stdin_open = false;
			}
			else
			{
				ptyoutpending += ret;
			}
		}

		// Close all unused endpoints & file descriptors
		for (;;)
		{
			// Close our output channels when the other input channels are closed (i.e. they're won't be any new data to send
			if (stdout_open && !stdoutpending && !ptyin_open)
			{
				if (!stdin_open)
					tcsetattr(STDOUT_FILENO, TCSADRAIN, origtty);
				close(STDOUT_FILENO);
				stdout_open = false;
				continue;
			}
			if (script_open && !scriptpending && !ptyin_open)
			{
				close(scriptfd);
				script_open = false;
				continue;
			}
			if (ptyout_open && !ptyoutpending && !stdin_open)
			{
				ptyout_open = false;
				if (!ptyin_open)
					close(pty);
				continue;
			}

			// Close our input channels when the respective output channels are closed
			if (stdin_open && (!ptyout_open || die))
			{
				if (!stdout_open)
					tcsetattr(STDIN_FILENO, TCSADRAIN, origtty);
				close(STDIN_FILENO);
				stdin_open = false;
				continue;
			}
			if (ptyin_open && !stdout_open)
			{
				ptyin_open = false;
				if (!ptyout_open)
					close(pty);
				continue;
			}

			break;
		}
	}

	if (eflg) {
		if (WIFSIGNALED(childstatus))
			exitcode = WTERMSIG(childstatus) + 0x80;
		else
			exitcode = WEXITSTATUS(childstatus);
	} else {
		exitcode = EX_OK;
	}

restoretty:
	// Restore terminal settings
	if      (stdin_open)
		tcsetattr(STDIN_FILENO, TCSADRAIN, origtty);
	else if (stdout_open)
		tcsetattr(STDOUT_FILENO, TCSADRAIN, origtty);

	return exitcode;
}

static void
finish(int dummy __attribute__ ((__unused__))) {
	int status;
	pid_t pid;

	while ((pid = wait3(&status, WNOHANG, 0)) > 0)
		if (pid == child) {
			childstatus = status;
			die = true;
		}
}

static void
resize(int dummy __attribute__ ((__unused__))) {
	/* transmit window change information to the child */
	struct winsize win;
	ioctl(0, TIOCGWINSZ, &win);
	ioctl(master, TIOCSWINSZ, &win);
	for (size_t written = 0; written < sizeof(win);)
	{
		const ssize_t ret = write(resizeout, ((const char*)&win) + written, sizeof(win) - written);
		if (ret == -1)
		{
			switch (errno)
			{
				case EINTR:
					continue;
				default:
					/* Can't safely kill ourselves from a
					 * signal handler, just ignore the
					 * error instead. Killing ourselves
					 * would leave the parent terminal in a
					 * bad state. */
					return;
			}
		}
	}
}

static int
doshell(const char* pts, const struct termios* origtty) {
	const int slave = getslave(pts, origtty);
	dup2(slave, STDIN_FILENO);
	dup2(slave, STDOUT_FILENO);
	dup2(slave, STDERR_FILENO);

	// Close slave filedescriptor only if it isn't one of the stdio descriptors
	if (slave != STDIN_FILENO
	 && slave != STDOUT_FILENO
	 && slave != STDERR_FILENO)
		close(slave);

	const char* shell = getenv("SHELL");
	if (shell == NULL)
		shell = _PATH_BSHELL;

	const char* shname = strrchr(shell, '/');
	if (shname)
		shname++;
	else
		shname = shell;

	if (cflg)
		execl(shell, shname, "-c", cflg, NULL);
	else
		execl(shell, shname, "-i", NULL);

	perror(shell);
	fail();
}

static void
fixtty(const struct termios* origtty) {
	struct termios rtt = *origtty;

	/* Set host terminal to raw mode. */
	rtt.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
	rtt.c_oflag &= ~OPOST;
	rtt.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	rtt.c_cflag &= ~(CSIZE | PARENB);
	rtt.c_cflag |= CS8;
	rtt.c_cc[VMIN] = 1; /* read returns when one char is available.  */
	rtt.c_cc[VTIME] = 0;
	tcsetattr(0, TCSANOW, &rtt);
}

static void
fail() {
	kill(0, SIGTERM);
	/* Shut up the compiler which thinks we'll get here (and thus return
	 * from a noreturn function). */
	for (;;)
		pause();
}

static void
getmaster() {
	master = open("/dev/ptmx", O_RDWR);
	if (master == -1
	 || grantpt(master) == -1
	 || unlockpt(master) == -1) {
		fprintf(stderr, _("opening a pty using /dev/ptmx failed\n"));
		fail();
	}

	int resizepipe[2];
	if (pipe(resizepipe) == -1) {
		fprintf(stderr, _("opening a pipe failed: %s\n"), strerror(errno));
		fail();
	}
	resizein  = resizepipe[0];
	resizeout = resizepipe[1];

	resize(0);
}

static int
getslave(const char* pts, const struct termios* origtty) {
	const int slave = open(pts, O_RDWR);

	if (slave == -1
	 || ioctl(slave, I_PUSH, "ptem") == -1   /* push ptem */
	 || ioctl(slave, I_PUSH, "ldterm") == -1 /* push ldterm*/
	 ) {
		perror(pts);
		fail();
	}

	// Copy tty-settings from parent terminal to client terminal
	tcsetattr(slave, TCSANOW, origtty);

	setsid();
	ioctl(slave, TIOCSCTTY, 0);

	return slave;
}
