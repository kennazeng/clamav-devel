/*
 *  Copyright (C) 2005 Nigel Horne <njh@bandsman.co.uk>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
static	char	const	rcsid[] = "$Id: pdf.c,v 1.19 2005/05/24 18:44:03 nigelhorne Exp $";

#if HAVE_CONFIG_H
#include "clamav-config.h"
#endif

#include "clamav.h"

#if HAVE_MMAP
#if HAVE_SYS_MMAN_H
#include <sys/mman.h>
#else /* HAVE_SYS_MMAN_H */
#undef HAVE_MMAP
#endif
#endif

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>

#ifdef HAVE_ZLIB_H
#include <zlib.h>
#endif

#include "table.h"
#include "mbox.h"
#include "others.h"
#include "blob.h"

#ifndef	MIN
#define	MIN(a, b)	(((a) < (b)) ? (a) : (b))
#endif

static	int	flatedecode(const unsigned char *buf, size_t len, int fout);
static	int	ascii85decode(const char *buf, size_t len, unsigned char *output);
static	const	char	*pdf_nextlinestart(const char *ptr, size_t len);

int
cli_pdf(const char *dir, int desc)
{
#ifndef HAVE_MMAP
	cli_warnmsg("File not decoded - PDF decoding needs mmap() (for now)\n");
	return CL_CLEAN;
#else
	struct stat statb;
	off_t size;
	long bytesleft, trailerlength;
	char *buf;
	const char *p, *q, *trailerstart;
	int rc = CL_CLEAN;

	cli_dbgmsg("in cli_pdf()\n");

	if(fstat(desc, &statb) < 0)
		return CL_EOPEN;

	size = (size_t)statb.st_size;

	if(size == 0)
		return CL_CLEAN;

	if(size <= 7)	/* doesn't even include the file header */
		return CL_EFORMAT;

	p = buf = mmap(NULL, size, PROT_READ, MAP_SHARED, desc, 0);
	if(buf == MAP_FAILED)
		return CL_EMEM;

	bytesleft = (long)size;

	cli_dbgmsg("cli_pdf: scanning %lu bytes\n", bytesleft);

	/* Lines are terminated by \r, \n or both */

	/* File Header */
	if(memcmp(p, "%PDF-1.", 7) != 0) {
		munmap(buf, size);
		return CL_EFORMAT;
	}

	q = pdf_nextlinestart(p, bytesleft);
	if(q == NULL) {
		munmap(buf, size);
		return CL_EFORMAT;
	}
	bytesleft -= (int)(q - p);
	p = q;

	/* Find the file trailer */
	for(q = &p[bytesleft - 1]; q > p; --q)
		if(memcmp(q, "%%EOF", 5) == 0)
			break;

	if(q == p) {
		munmap(buf, size);
		return CL_EFORMAT;
	}

	for(trailerstart = q; trailerstart > p; --trailerstart)
		if(memcmp(trailerstart, "trailer", 7) == 0)
			break;

	/*
	 * q points to the end of the trailer section
	 */
	trailerlength = (long)(q - trailerstart);
	if(cli_pmemstr(trailerstart, trailerlength, "Encrypt", 7)) {
		/*
		 * This tends to mean that the file is, in effect, read-only
		 */
		cli_warnmsg("Encrypted PDF files not yet supported\n");
		munmap(buf, size);

		return CL_EFORMAT;
	}

	bytesleft -= trailerlength;

	while((q = cli_pmemstr(p, bytesleft, " obj", 4)) != NULL) {
		int is_ascii85decode, is_flatedecode, fout, len;
		const char *s, *streamstart, *u, *v, *objstart;
		size_t length, objlen, streamlen;
		char fullname[NAME_MAX + 1];

		bytesleft -= (q - p) + 4;
		objstart = p = &q[4];
		q = cli_pmemstr(p, bytesleft, "endobj", 6);
		if(q == NULL) {
			cli_dbgmsg("No matching endobj");
			break;
		}
		bytesleft -= (q - p) + 6;
		p = &q[6];
		objlen = (size_t)(q - objstart);

		streamstart = cli_pmemstr(objstart, objlen, "stream", 6);
		if(streamstart == NULL)
			continue;

		length = is_ascii85decode = is_flatedecode = 0;
		/*
		 * TODO: handle F and FFilter?
		 */
		for(s = objstart; s < streamstart; s++)
			if(*s == '/') {
				if(strncmp(++s, "Length ", 7) == 0) {
					s += 7;
					length = atoi(s);
					while(isdigit(*s))
						s++;
				} else if((strncmp(s, "FlateDecode ", 12) == 0) ||
					  (strncmp(s, "FlateDecode\n", 12) == 0)) {
					is_flatedecode = 1;
					s += 12;
				} else if((strncmp(s, "ASCII85Decode ", 13) == 0) ||
					  (strncmp(s, "ASCII85Decode\n", 13) == 0)) {
					is_ascii85decode = 1;
					s += 12;
				}
			}

		/* q points to the end of the object (objend) */
		streamstart += 6;
		len = (int)(q - streamstart);
		u = pdf_nextlinestart(streamstart, len);
		if(u == NULL)
			break;
		len -= (int)(u - streamstart);
		streamstart = u;
		u = cli_pmemstr(streamstart, len, "endstream\n", 10);
		if(u == NULL) {
			u = cli_pmemstr(streamstart, len, "endstream\r", 10);
			if(u == NULL) {
				cli_dbgmsg("No endstream");
				break;
			}
		}
		v = u;
		while(strchr("\r\n", *--v))
			--u;
		snprintf(fullname, sizeof(fullname), "%s/pdfXXXXXX", dir);
#if	defined(C_LINUX) || defined(C_BSD) || defined(HAVE_MKSTEMP) || defined(C_SOLARIS) || defined(C_CYGWIN)
		fout = mkstemp(fullname);
#else
		(void)mktemp(fullname);
		fout = open(fullname, O_WRONLY|O_CREAT|O_EXCL|O_TRUNC|O_BINARY, 0600);
#endif

		if(fout < 0) {
			cli_errmsg("cli_pdf: can't create temporary file %s: %s\n", fullname, strerror(errno));
			rc = CL_ETMPFILE;
			break;
		}

		streamlen = (int)(u - streamstart) + 1;

		/*cli_dbgmsg("length %d, streamlen %d\n", length, streamlen);*/

#if	0
		/* FIXME: this isn't right... */
		if(length)
			/*streamlen = (is_flatedecode) ? length : MIN(length, streamlen);*/
			streamlen = MIN(length, streamlen);
#endif

		if(is_ascii85decode) {
			unsigned char *tmpbuf = cli_malloc(streamlen * 2);
			int ret;

			if(tmpbuf == NULL) {
				rc = CL_EMEM;
				continue;
			}

			ret = ascii85decode(streamstart, streamlen, tmpbuf);

			if(ret == -1) {
				free(tmpbuf);
				rc = CL_EFORMAT;
				continue;
			}
			streamlen = (size_t)ret;
			/* free unused traling bytes */
			tmpbuf = cli_realloc(tmpbuf, streamlen);
			/*
			 * Note that it will probably be both ascii85encoded
			 * and flateencoded
			 */
			if(is_flatedecode) {
				const int zstat = flatedecode((unsigned char *)tmpbuf, streamlen, fout);

				if(zstat != Z_OK)
					rc = CL_EZIP;
			}
			free(tmpbuf);
		} else if(is_flatedecode) {
			const int zstat = flatedecode((unsigned char *)streamstart, streamlen, fout);

			if(zstat != Z_OK)
				rc = CL_EZIP;
		} else
			write(fout, streamstart, streamlen);

		close(fout);
		cli_dbgmsg("cli_pdf: extracted to %s\n", fullname);
	}

	munmap(buf, size);

	cli_dbgmsg("cli_pdf: returning %d\n", rc);
	return rc;
#endif
}

/* flate inflation - returns zlib status, e.g. Z_OK */
static int
flatedecode(const unsigned char *buf, size_t len, int fout)
{
	int zstat;
	z_stream stream;
	unsigned char output[BUFSIZ];

	cli_dbgmsg("cli_pdf: flatedecode %lu bytes\n", len);

	while(strchr("\r\n", *buf)) {
		len--;
		buf++;
	}

	stream.zalloc = (alloc_func)Z_NULL;
	stream.zfree = (free_func)Z_NULL;
	stream.opaque = (void *)NULL;
	stream.next_in = (unsigned char *)buf;
	stream.avail_in = len;
	stream.next_out = output;
	stream.avail_out = sizeof(output);

	zstat = inflateInit(&stream);
	if(zstat != Z_OK) {
		cli_warnmsg("cli_pdf: inflateInit failed");
		return zstat;
	}
	for(;;) {
		zstat = inflate(&stream, Z_NO_FLUSH);
		switch(zstat) {
			case Z_OK:
				if(stream.avail_out == 0) {
					write(fout, output, sizeof(output));
					stream.next_out = output;
					stream.avail_out = sizeof(output);
				}
				continue;
			case Z_STREAM_END:
				break;
			default:
				if(stream.msg)
					cli_warnmsg("Error \"%s\" inflating PDF attachment\n", stream.msg);
				else
					cli_warnmsg("Error %d inflating PDF attachment\n", zstat);
				inflateEnd(&stream);
				return zstat;
		}
		break;
	}

	if(stream.avail_out != sizeof(output))
		write(fout, output, sizeof(output) - stream.avail_out);
	return inflateEnd(&stream);
}

/*
 * http://cvs.gnome.org/viewcvs/sketch/Filter/ascii85filter.c?rev=1.2
 */
/* ascii85 inflation, returns number of bytes in output, -1 for error */
static int
ascii85decode(const char *buf, size_t len, unsigned char *output)
{
	const char *ptr = buf;
	uint32_t sum = 0;
	int quintet = 0;
	int ret = 0;

	cli_dbgmsg("cli_pdf: ascii85decode %lu bytes\n", len);

	while(len > 0) {
		int byte = (len--) ? (int)*ptr++ : EOF;

		if((byte == '~') && (*ptr == '>'))
			byte = EOF;

		if(byte >= '!' && byte <= 'u') {
			sum = sum * 85 + ((unsigned long)byte - '!');
			if(++quintet == 5) {
				*output++ = sum >> 24;
				*output++ = (sum >> 16) & 0xFF;
				*output++ = (sum >> 8) & 0xFF;
				*output++ = sum & 0xFF;
				ret += 4;
				quintet = 0;
				sum = 0;
			}
		} else if(byte == 'z') {
			if(quintet) {
				cli_warnmsg("ascii85decode: unexpected 'z'\n");
				return -1;
			}
			*output++ = '\0';
			*output++ = '\0';
			*output++ = '\0';
			*output++ = '\0';
			ret += 4;
		} else if(byte == EOF) {
			if(quintet) {
				int i;

				if(quintet == 1) {
					cli_warnmsg("ascii85Decode: only 1 byte in last quintet\n");
					return -1;
				}
				for(i = 0; i < 5 - quintet; i++)
					sum *= 85;
				if(quintet > 1)
					sum += (0xFFFFFF >> ((quintet - 2) * 8));
				ret += quintet;
				for(i = 0; i < quintet - 1; i++)
					*output++ = (sum >> (24 - 8 * i)) & 0xFF;
				quintet = 0;
			}
			break;
		} else if(!isspace(byte)) {
			cli_warnmsg("ascii85Decode: invalid character 0x%x, len %lu\n", byte, len);
			return -1;
		}
	}
	return ret;
}

/*
 * Find the start of the next line
 */
static const char *
pdf_nextlinestart(const char *ptr, size_t len)
{
	while(strchr("\r\n", *ptr) == NULL) {
		if(--len == 0L)
			return NULL;
		ptr++;
	}
	while(strchr("\r\n", *ptr) != NULL) {
		if(--len == 0L)
			return NULL;
		ptr++;
	}
	return ptr;
}
