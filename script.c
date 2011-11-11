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
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/file.h>
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

void finish(int);
void done(void);
static void fail(void);
void resize(int);
static void fixtty(const struct termios*);
void getmaster(void);
static int getslave(int master, const struct termios* origtty);
static void doinput(const struct termios* origtty);
void dooutput(void);
static int doshell(const struct termios* origtty);

static int master = -1;
static pid_t child;
static int childstatus;
static const char* fname;

int	lb;
int	l;
int	aflg = 0;
char	*cflg = NULL;
int	eflg = 0;
int	fflg = 0;
int	qflg = 0;
int	tflg = 0;

static char *progname;

int die;
int resized;

static void
die_if_link(char *fn) {
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
	struct sigaction sa;
	extern int optind;
	char *p;
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
			exit(1);
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

	struct termios origtty;
	tcgetattr(STDIN_FILENO, &origtty);
	fixtty(&origtty);

	/* setup SIGCHLD handler */
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sa.sa_handler = finish;
	sigaction(SIGCHLD, &sa, NULL);

	/* init mask for SIGCHLD */
	sigprocmask(SIG_SETMASK, NULL, &block_mask);
	sigaddset(&block_mask, SIGCHLD);

	sigprocmask(SIG_SETMASK, &block_mask, &unblock_mask);
	child = fork();
	sigprocmask(SIG_SETMASK, &unblock_mask, NULL);

	if (child < 0) {
		perror("fork");
		fail();
	}
	if (child == 0) {

		sigprocmask(SIG_SETMASK, &block_mask, NULL);
		child = fork();
		sigprocmask(SIG_SETMASK, &unblock_mask, NULL);

		if (child < 0) {
			perror("fork");
			fail();
		}
		if (child)
			dooutput();
		else
			doshell(&origtty);
	} else {
		sa.sa_handler = resize;
		sigaction(SIGWINCH, &sa, NULL);
	}
	doinput(&origtty);

	return 0;
}

void
doinput(const struct termios* origtty) {
	register int cc;
	char ibuf[BUFSIZ];

	(void) close(1);

	while (die == 0) {
		if ((cc = read(0, ibuf, BUFSIZ)) > 0) {
			ssize_t wrt = write(master, ibuf, cc);
			if (wrt == -1) {
				int err = errno;
				fprintf (stderr, _("%s: write error %d: %s\n"),
					progname, err, strerror(err));
				tcsetattr(STDIN_FILENO, TCSADRAIN, origtty);
				fail();
			}
		}
		else if (cc == -1 && errno == EINTR && resized)
			resized = 0;
		else
			break;
	}

	tcsetattr(STDIN_FILENO, TCSADRAIN, origtty);
	if (!qflg)
		printf(_("Script done, file is %s\n"), fname);
	done();
}

#include <sys/wait.h>

void
finish(int dummy __attribute__ ((__unused__))) {
	int status;
	register int pid;

	while ((pid = wait3(&status, WNOHANG, 0)) > 0)
		if (pid == child) {
			childstatus = status;
			die = 1;
		}
}

void
resize(int dummy __attribute__ ((__unused__))) {
	resized = 1;
	/* transmit window change information to the child */
	struct winsize win;
	(void) ioctl(0, TIOCGWINSZ, &win);
	(void) ioctl(master, TIOCSWINSZ, &win);
}

void
dooutput() {
	register ssize_t cc;
	char obuf[BUFSIZ];
	int flgs = 0;
	ssize_t wrt;
	ssize_t fwrt;

	(void) close(0);

	FILE* const fscript = fopen(fname, aflg ? "a" : "w");
	if (fscript == NULL) {
		perror(fname);
		fail();
	}

	struct timeval oldtime, newtime;
	gettimeofday(&newtime, NULL);
	oldtime = newtime;

	strftime(obuf, sizeof(obuf), "%Y-%m-%d %H:%M:%S %Z", localtime(&newtime.tv_sec));
	fprintf(fscript, _("Script started on %s\n"), obuf);

	do {
		if (die && flgs == 0) {
			/* ..child is dead, but it doesn't mean that there is
			 * nothing in buffers.
			 */
			flgs = fcntl(master, F_GETFL, 0);
			if (fcntl(master, F_SETFL, (flgs | O_NONBLOCK)) == -1)
				break;
		}

		errno = 0;
		cc = read(master, obuf, sizeof (obuf));
		if (tflg)
			gettimeofday(&newtime, NULL);

		if (die && errno == EINTR && cc <= 0)
			/* read() has been interrupted by SIGCHLD, try it again
			 * with O_NONBLOCK
			 */
			continue;
		if (cc <= 0)
			break;
		if (tflg) {
			const int usec_compensation = (newtime.tv_usec > oldtime.tv_usec) ? 0 : 1;
			const struct timeval diff = {
				.tv_sec  = newtime.tv_sec  - oldtime.tv_sec  - usec_compensation,
				.tv_usec = newtime.tv_usec - oldtime.tv_usec + usec_compensation * 1000000L,
			};
			fprintf(stderr, "%03lld.%06ld %zd\n", (long long)diff.tv_sec, (long)diff.tv_usec, cc);
			oldtime = newtime;
		}
		wrt = write(1, obuf, cc);
		if (wrt < 0) {
			int err = errno;
			fprintf (stderr, _("%s: write error: %s\n"),
				progname, strerror(err));
			fail();
		}
		fwrt = fwrite(obuf, 1, cc, fscript);
		if (fwrt < cc) {
			int err = errno;
			fprintf (stderr, _("%s: cannot write script file, error: %s\n"),
				progname, strerror(err));
			fail();
		}
		if (fflg)
			(void) fflush(fscript);
	} while(1);

	if (flgs)
		fcntl(master, F_SETFL, flgs);
	if (!qflg) {
		if (!tflg)
			gettimeofday(&newtime, NULL);
		strftime(obuf, sizeof(obuf), "%Y-%m-%d %H:%M:%S %Z", localtime(&newtime.tv_sec));
		fprintf(fscript, _("\nScript done on %s\n"), obuf);
	}
	fclose(fscript);
	done();
}

static int
doshell(const struct termios* origtty)
{
#if 0
	int t = open(_PATH_DEV_TTY, O_RDWR);
	if (t >= 0) {
		(void) ioctl(t, TIOCNOTTY, (char *)0);
		(void) close(t);
	}
#endif

	const int slave = getslave(master, origtty);
	(void) close(master);
	(void) dup2(slave, 0);
	(void) dup2(slave, 1);
	(void) dup2(slave, 2);
	(void) close(slave);

	master = -1;

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

	return EX_OSERR;
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
	(void) tcsetattr(0, TCSANOW, &rtt);
}

static void
fail() {

	(void) kill(0, SIGTERM);
	done();
}

void
done() {
	if (eflg) {
		if (WIFSIGNALED(childstatus))
			exit(WTERMSIG(childstatus) + 0x80);
		else
			exit(WEXITSTATUS(childstatus));
	}
	exit(0);
}

void
getmaster() {
	master = open("/dev/ptmx", O_RDWR);
	if (master == -1
	 || grantpt(master) == -1
	 || unlockpt(master) == -1) {
		fprintf(stderr, _("opening a pty using /dev/ptmx failed\n"));
		fail();
	}

	resize(0);
}

static int
getslave(const int master, const struct termios* origtty) {
	const char* name = ptsname(master);
	const int slave = name ? open(name, O_RDWR) : -1;

	if (slave == -1
	 || ioctl(slave, I_PUSH, "ptem") == -1   /* push ptem */
	 || ioctl(slave, I_PUSH, "ldterm") == -1 /* push ldterm*/
	 ) {
		perror(name ? name : "ptsname");
		fail();
	}

	// Copy tty-settings from parent terminal to client terminal
	tcsetattr(slave, TCSANOW, origtty);

	(void) setsid();
	(void) ioctl(slave, TIOCSCTTY, 0);

	return slave;
}
