/*
 * Copyright (C) 2008, Karel Zak <kzak@redhat.com>
 * Copyright (C) 2008, James Youngman <jay@gnu.org>
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 *
 * Based on scriptreplay.pl by Joey Hess <joey@kitenet.net>
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <limits.h>
#include <math.h>
#include <sys/select.h>
#include <fcntl.h>
#include <unistd.h>
#include <locale.h>

#define _(Text) (Text)

#define SCRIPT_MIN_DELAY 0.0001		/* from original sripreplay.pl */

#define MAX(a,b) ((a) < (b) ? (b) : (a))
#define MIN(a,b) ((a) < (b) ? (a) : (b))

static const char* program_invocation_short_name;

void __attribute__((__noreturn__))
usage(int rc)
{
	printf(_("%s <timingfile> [<typescript> [<divisor>]]\n"),
			program_invocation_short_name);
	exit(rc);
}

static void err(int eval, const char* fmt, ...)
{
	int err = errno;

	if (fmt)
	{
		va_list ap;
		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		va_end(ap);
		fputs(": ", stderr);
	}

	fprintf(stderr, "%s\n", strerror(err));
	exit(eval);
}

static void errx(int eval, const char* fmt, ...)
{
	if (fmt)
	{
		va_list ap;
		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		va_end(ap);
		fputs("\n", stderr);
	}

	exit(eval);
}

static double
getnum(const char *s)
{
	double d;
	char *end;

	errno = 0;
	d = strtod(s, &end);

	if (end && *end != '\0')
		errx(EXIT_FAILURE, _("expected a number, but got '%s'"), s);

	if ((d == HUGE_VAL || d == -HUGE_VAL) && ERANGE == errno)
		err(EXIT_FAILURE, _("divisor '%s'"), s);

	if (!(d==d)) { /* did they specify "nan"? */
		errno = EINVAL;
		err(EXIT_FAILURE, _("divisor '%s'"), s);
	}
	return d;
}

static void
delay_for(double delay)
{
	if (delay < SCRIPT_MIN_DELAY)
		return;
#ifdef HAVE_NANOSLEEP
	struct timespec ts, remainder;
	ts.tv_sec = (time_t) delay;
	ts.tv_nsec = (delay - ts.tv_sec) * 1.0e9;

	while (-1 == nanosleep(&ts, &remainder)) {
		if (EINTR == errno)
			ts = remainder;
		else
			break;
	}
#else
	struct timeval tv;
	tv.tv_sec = (long) delay;
	tv.tv_usec = (delay - tv.tv_sec) * 1.0e6;
	select(0, NULL, NULL, NULL, &tv);
#endif
}

static void
bufflush(char* buf, size_t* outpending, size_t processing)
{
	while (*outpending)
	{
		const ssize_t written = write(STDOUT_FILENO, buf, *outpending);
		if (written == -1)
		{
			switch (errno)
			{
				case EINTR:
					continue;
				default:
					err(EXIT_FAILURE, _("Failed to write to stdout"));
			}
		}

		*outpending -= written;
		memmove(buf, buf + written, *outpending + processing);
	}
}

static void
emit(const int fd, const char *const filename, size_t ct, const double divi)
{
	bool eof = false;

	char buf[65536];
	size_t inpending = 0;
	size_t outpending = 0;

	char apc_delay_buf[65536];
	size_t apc_delay_len = 0;
	int apc_delay_state = 0;
	while (ct || inpending || outpending)
	{
		const size_t to_read = MIN(ct, sizeof(buf) - inpending - outpending);
		const ssize_t ret = eof ? 0 : read(fd, buf, to_read);
		if (ret == -1)
			err(EXIT_FAILURE, _("Unexpected error while reading %s"), filename);
		else if (ret == 0)
			eof = true;
		inpending += ret;
		if (ct != (size_t)-1)
			ct -= ret;

		if (outpending + inpending == 0
		 && !ret)
			break;

		while (inpending)
		{
			--inpending;
			const char c = buf[outpending + apc_delay_state + apc_delay_len];

			if     ((c == 0x1B && apc_delay_state == 0) /* | APC sequence */
			     || (c == '_'  && apc_delay_state == 1) /* |              */
			     || (c == 'D'  && apc_delay_state == 2)
			     || (c == ';'  && apc_delay_state == 3))
			{
				++apc_delay_state;
			}
			else if (c != 0x1B && apc_delay_state == 4)
			{
				apc_delay_buf[apc_delay_len++] = c; // Actual APC delay-command content
			}
			else if (c == 0x1B && apc_delay_state == 4) /* | ST sequence  */
			{
				++apc_delay_state;
			}
			else if (c == '\\' && apc_delay_state == 5) /* |              */
			{
				/* Properly formed APC delay-command, process it. */
				apc_delay_buf[apc_delay_len++] = '\0';
				char* end;
				double delay = strtod(apc_delay_buf, &end);
				if (&apc_delay_buf[apc_delay_len-1] == end)
				{
					bufflush(buf, &outpending, apc_delay_state + apc_delay_len + inpending);
					delay_for(delay / divi);
					// Remove APC delay-command from buffer
					memmove(buf, buf + apc_delay_state + apc_delay_len, inpending);
				}
				else
				{
					outpending += apc_delay_state + apc_delay_len;
				}
				apc_delay_len = 0;
				apc_delay_state = 0;
			}
			else if (!apc_delay_state && !apc_delay_len)
			{
				++outpending;
			}
			else
			{
				outpending += apc_delay_state + apc_delay_len + 1;
				apc_delay_len = 0;
				apc_delay_state = 0;
			}
		}

		if ((outpending + inpending) >= (sizeof(buf) - sizeof("18446744073709551615.999999"))
		 || !inpending)
			bufflush(buf, &outpending, apc_delay_state + apc_delay_len + inpending);
	}

	if (!ct || ct == (size_t)-1)
		return;
	if (eof)
		errx(EXIT_FAILURE, _("unexpected end of file on %s (%zu, %zu, %zu)"), filename, ct, outpending, inpending);

	err(EXIT_FAILURE, _("failed to read typescript file %s"), filename);
}


int
main(int argc, char *argv[])
{
	FILE *tfile;
	const char *sname, *tname;
	double divi;
	int c;
	unsigned long line;
	size_t oldblk = 0;

	program_invocation_short_name = argv[0];

	/* Because we use space as a separator, we can't afford to use any
	 * locale which tolerates a space in a number.  In any case, script.c
	 * sets the LC_NUMERIC locale to C, anyway.
	 */
	setlocale(LC_ALL, "");
	setlocale(LC_NUMERIC, "C");

	if (argc > 4)
		usage(EXIT_FAILURE);
	if (argc < 2
	 && (isatty(STDIN_FILENO)
	  || errno == EBADF))
		usage(EXIT_FAILURE);

	tname = (argc < 2) ? "stdin" : argv[1];
	sname = argc > 2 ? argv[2] : "typescript";
	divi = argc == 4 ? getnum(argv[3]) : 1;

	tfile = (argc < 2) ? stdin : fopen(tname, "r");
	if (!tfile)
		err(EXIT_FAILURE, _("cannot open timing file %s"), tname);
	int sfile = (argc < 2) ? -1 : open(sname, O_RDONLY);
	if (sfile == -1)
	{
		sfile = fileno(tfile);
		sfile = (argc < 2) ? sfile : dup(sfile);
		if (sfile == -1)
			err(EXIT_FAILURE, _("dup(2) failed for %s"), tname);
		sname = tname;
		if (argc >= 2)
			fclose(tfile);
		tfile = NULL;
		tname = NULL;
		divi = argc == 3 ? getnum(argv[2]) : 1;

		off_t size = lseek(sfile, 0, SEEK_END);
		if (size == (off_t)-1 && argc >= 2)
		{
			err(EXIT_FAILURE, _("failure to determine typescript file size %s"), sname);
		}
		else if (size != (off_t)-1)
		{
			oldblk = size;
			if (lseek(sfile, 0, SEEK_SET) == (off_t)-1)
				err(EXIT_FAILURE, _("failure to seek to start of typescript %s"), sname);
		}
		else
		{
			oldblk = (size_t)-1;
		}
	}

	/* ignore the first typescript line */
	char ci;
	while ((c = read(sfile, &ci, sizeof(ci))) == sizeof(ci) && ci != '\n')
		;
	if (c == -1)
		err(EXIT_FAILURE, _("Failed to read from %s"), sname);

	if (oldblk && oldblk != (size_t)-1)
		oldblk -= lseek(sfile, 0, SEEK_CUR);

	for(line = 0; tfile || oldblk; line++) {
		double delay = 0;
		size_t blk = 0;

		if (tfile
		 && fscanf(tfile, "%lf %zu\n", &delay, &blk) != 2) {
			if (feof(tfile))
				break;
			if (ferror(tfile))
				err(EXIT_FAILURE,
					_("failed to read timing file %s"), tname);
			errx(EXIT_FAILURE,
				_("timings file %s: %lu: unexpected format"),
				tname, line);
		}
		delay_for(delay / divi);

		if (oldblk)
			emit(sfile, sname, oldblk, divi);
		oldblk = blk;
	}

	if (tfile)
		fclose(tfile);
	exit(EXIT_SUCCESS);
}
