/*
   Copyright (C) 2021 Free Software Foundation, Inc.
   Written by Ron Norman

   This file is part of GnuCOBOL.

   The GnuCOBOL cobfile program is free software: you can redistribute it
   and/or modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation, either version 3 of the
   License, or (at your option) any later version.

   GnuCOBOL is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with GnuCOBOL.  If not, see <https://www.gnu.org/licenses/>.
*/

/*
     Program:  cobfile.c

     Function: This program is used to create/execute 
	 			file copy/convert programs.
*/

#include	"config.h"
#include	<stdio.h>
#include	<stdlib.h>
#ifdef	HAVE_UNISTD_H
#include	<unistd.h>
#endif
#include	<string.h> 
#include	<time.h> 
#ifdef	HAVE_SYS_TIME_H
#include	<sys/time.h>
#endif
#include	<sys/types.h>
#include	<sys/stat.h>
#include	<ctype.h>
#include	<libcob.h>
#include	<stdarg.h>
#include	"tarstamp.h"
#include	"libcob/cobgetopt.h"
#include	"libcob/sysdefines.h"
#if defined(HAVE_READLINE) || defined(__linux__)
#include <readline/readline.h>
#include <readline/history.h>
#endif

/* needed for time checks */
#ifdef	HAVE_LOCALE_H
#include <locale.h>
#endif


#if	defined(ENABLE_NLS) && defined(COB_NLS_RUNTIME)
#include	"defaults.h" /* get LOCALEDIR */
#include "gettext.h"	/* from lib/ */
#define _(s)		gettext(s)
#define N_(s)		gettext_noop(s)
#else
#define _(s)		s
#define N_(s)		s
#endif


#define TRUE 1
#define FALSE 0
static int be_quiet = 0;
static int keep_code = 0;
static int batchin = 0;
static const cob_field_attr	const_alpha_attr =
				{COB_TYPE_ALPHANUMERIC, 0, 0, 0, NULL};
static const cob_field_attr	all_numeric_display_attr =
				{COB_TYPE_NUMERIC_DISPLAY, COB_MAX_DIGITS, 0,
				 0, NULL};
static char prompt[64] = "cobfile:>";
static char	cmdfile[256] = "";
static char	progid[32] = "";
static char copyin[256] = "";
static char copyout[256] = "";
static char fileindef[256] = "";
static const char *copyext[] = {"",".cpy",".cbl",".cob",".CPY",".CBL",".COB",NULL};
static char *copysearch = NULL;
#if 0 /* For later use */
static int outNl = 0;
static FILE	*fo = NULL;
/********************************/
/**  Source code for emission  **/
/********************************/
typedef struct {
	char		type;	/* type of statement being processed */
	const char	*text;	/* Text to be emitted */
} Source;

static Source ident[] = {
	{0," IDENTIFICATION DIVISION.\n"},
	{0," \n"},
	{0,NULL}};
#endif

static const char short_options[] = "hqVki:p:D:";

#define	CB_NO_ARG	no_argument
#define	CB_RQ_ARG	required_argument
#define	CB_OP_ARG	optional_argument

static const struct option long_options[] = {
	{"in",			CB_RQ_ARG, NULL, 'i'},
	{"prog",		CB_RQ_ARG, NULL, 'p'},
	{"define",		CB_RQ_ARG, NULL, 'D'},
	{"keep",		CB_NO_ARG, NULL, 'k'},
	{"help",		CB_NO_ARG, NULL, 'h'},
	{"quiet",		CB_NO_ARG, NULL, 'q'},
	{"version",     CB_NO_ARG, NULL, 'V'},
	{NULL, 0, NULL, 0}
};

/* Remove trailing CR, LF and spaces */
static int
trim_line(char *buf)
{
	int	j,k;
	k = strlen(buf);
	while (k > 0
		&& (buf[k-1] == '\r' || buf[k-1] == '\n')) {
		buf[--k] = 0;
	}
	while (k > 0
		&& buf[k-1] == ' ') {
		buf[--k] = 0;
	}
	for (j=k=0; buf[k] != 0; ) {
		if (buf[k] == ' ' && buf[k+1] == ' ') {
			k++;
		} else if (j==0 && buf[k] == ' ') {
			k++;
		} else {
			buf[j++] = buf[k++];
		}
	}
	buf[j] = 0;
	return k;
}

/* Read a line from stdin */
static char *
getLine (char *buf)
{
	int		k;
	char	ibuf[1024];
getnext:
#if defined(HAVE_READLINE) || defined(__linux__)
	{
		char	*p;
		if (batchin) {
			if (fgets (ibuf, sizeof(ibuf)-1, stdin) == NULL)
				return NULL;
			if (ibuf[0] == '*' 
			 || ibuf[0] == '#')
				goto getnext;
		} else {
			if ((p = readline (prompt)) == NULL)
				return NULL;
			strcpy (ibuf,p);
			free (p);
			if (ibuf[0] == '*' 
			 || ibuf[0] == '#')
				goto getnext;
			if (ibuf[0] >= ' ')
				add_history (ibuf);
		}
	}
#else
	if (!batchin) {
		fputs (prompt, stdout);
		fflush(stdout);
	}
	if (fgets (ibuf, sizeof(ibuf)-1, stdin) == NULL)
		return NULL;
	if (ibuf[0] == '*' 
	 || ibuf[0] == '#')
		goto getnext;
#endif
	trim_line (ibuf);
	for (k=0; memcmp(&ibuf[k],"  ",2)==0; k++);
	strcpy (buf, &ibuf[k]);
	return buf;
}

/* Make a new 'cob_field' */
static cob_field *
makeField (int size)
{
	cob_field *f;

	f = calloc (1, sizeof(cob_field));
	f->size = size;
	f->attr = &const_alpha_attr;
	f->data = calloc (1, size);
	return f;
}

/* Make a new 'cob_field' */
static cob_field *
makeField9 (int size)
{
	cob_field *f;

	f = calloc (1, sizeof(cob_field));
	f->size = size;
	f->attr = &all_numeric_display_attr;
	f->data = calloc (1, size);
	return f;
}

/* Drop a 'cob_field' */
static void
dropField (cob_field *f)
{
	if (f == NULL)
		return;
	if (f->data)
		free ((void*)f->data);
	free ((void*)f);
}

/* Does the 'word' match within the string */
static int
matchWord (const char *word, char *defs, char *str, int *pos )
{
	int	j, k;
	int	ln = strlen (word);
	char	*newstr;
	if (strncasecmp (defs + *pos, word, ln) != 0)
		return 0;
	k = *pos + ln;
	while (defs[k] == ' ') k++;
	if (defs[k] != '=')
		return 0;
	k++;
	while (defs[k] == ' ') k++;
	for (j = 0; defs[k] > ' ' 
			&& defs[k] != ',' 
			&& defs[k] != ';' 
			&& defs[k] != 0; )
		str[j++] = defs[k++];
	str[j] = 0;
	memset (defs + *pos, ' ', k - *pos);
	*pos = k - 1;
	newstr = cob_expand_env_string (str);
	strcpy (str, newstr);
	cob_free (newstr);
	return 1;
}

/* Find the COPY book */
static char *
findCopy (char *book)
{
	char	path[512], *p, *t;
	int		k,pl;
	FILE	*fi;

	if (book[0] == '.' || book[0] == SLASH_CHAR) {
		fi = fopen (path, "r");
		if (fi != NULL) {
			strcpy (book, path);
			fclose (fi);
			return book;
		}
	}
	for (p = copysearch; p && *p != 0; p = t) {
		t = strchr (p, PATHSEP_CHAR);
		if (t == NULL) {
			pl = sprintf (path, "%s", p);
			t = p + pl;
		} else {
			pl = sprintf (path, "%.*s", (int)(t-p), p);
			t++;
		}
		for (k = 0; copyext[k] != NULL; k++) {
			sprintf(&path[pl],"%c%s%s",SLASH_CHAR,book,copyext[k]);
			fi = fopen (path, "r");
			if (fi != NULL) {
				strcpy (book, path);
				fclose (fi);
				return book;
			}
		}
	}
	return NULL;
}

static void
getRecsz (cob_file *fl, char *def)
{
	int	k, rcsz;
	for (k=0; def[k] != 0; k++) {
		if (strncasecmp (&def[k], "recsz=",6) == 0) {
			rcsz = atoi (&def[k+6]);
			if (fl->record_max != rcsz) {
				fl->record_max = rcsz;
				fl->record_min = rcsz;
			}
			break;
		}
		if (strncasecmp (&def[k], "maxsz=",6) == 0) {
			rcsz = atoi (&def[k+6]);
			if (fl->record_max != rcsz) {
				fl->record_max = rcsz;
			}
			break;
		}
	}
}

/* Parse out the file definition */
static void
parseFile (cob_file *fl, const char *select, int rcsz, char *defs, char *copy)
{
	char	val[1024], both[1024], filename[1024];
	int		j,k,ln;
	cob_field *flsts;

	strcpy (copy, "");
	fl->select_name = strdup (select);
	fl->file_version = COB_FILE_VERSION;
	fl->organization = COB_ORG_SEQUENTIAL;
	fl->access_mode = COB_ACCESS_SEQUENTIAL;
	for (k=0; defs[k] != 0; k++) {
		if (k==0 
		|| defs[k-1] == ' ') {
			if (matchWord ("FILE", defs, val, &k)) {
				ln = strlen (val);
				dropField (fl->assign);
				fl->assign = makeField (ln + 2);
				memcpy(fl->assign->data, val, ln);
				fl->assign->data[ln] = 0;
			} else
			if (matchWord ("COPY", defs, val, &k)) {
				if (findCopy (val)) {
					strcpy (copy, val);
				} else {
				}
			}
		}
	}
	for (j=k=0; defs[k] != 0; ) {
		if (defs[k] == ' ' && defs[k+1] == ' ') {
			k++;
		} else {
			defs[j++] = defs[k++];
		}
	}
	defs[j] = 0;
	fl->record_max = rcsz;
	getRecsz (fl, defs);

	if (strcmp (select,"INPUT") == 0) {
		cob_pre_open_def (fl, defs, fileindef, 1);
		getRecsz (fl, fileindef);
	} else if (fileindef[0] > ' ') {
		fl->nkeys = 0;
		getRecsz (fl, fileindef);
		sprintf(both,"%s %s",fileindef, defs);
		cob_pre_open_def (fl, both, val, 0);
		if (fl->assign
		 && fl->assign->data) {
			flsts = makeField (2);
			cob_delete_file (fl, flsts, 0);
			dropField (flsts);
			sprintf(filename,"%s.dat",fl->assign->data);
			unlink (filename);
			sprintf(filename,"%s.idx",fl->assign->data);
			unlink (filename);
		}
		getRecsz (fl, val);
	} else {
		cob_pre_open_def (fl, defs, NULL, 0);
	}
}

#if 0 /* For later use */
/*****************************************************************/
/* "output" a formated string to the output file                 */
/*****************************************************************/
static void
output(
	char *fmt, ...)
{
	va_list ap;
	char buf[ 300 ];
	int	i;

	va_start( ap, fmt );
	vsprintf( buf, fmt, ap );

	if(outNl
	&& buf[0] != '\n') {
		fprintf(fo,"%6s"," ");
	}
	i = 0;
	if(buf[i] == '!') {
		fputs("     ", fo);	/* Skip over to AREA B */
		i++;
	}
	if(buf[i] == '^') {
		fputs("   ", fo);	/* Skip over a few */
		i++;
	}
	while(buf[i] == '\t') {	/* Tab = move over 4 spaces */
		fputs("    ", fo);
		i++;
	}
	fputs(&buf[i], fo);

	i = strlen(buf);
	if(buf[i-1] == '\n')
		outNl = TRUE;
	else
		outNl = FALSE;

	va_end( ap );
	return;
}

/*****************************************************************/
/** Write an array of strings to the output file                **/
/*****************************************************************/
static void
outArray(
	Source	*array)
{
	int		i;
	char	buf[128];
	for(i=0; array[i].text != NULL; i++) {
		if(strlen(array[i].text) < 1)
			continue;
		strcpy(buf,array[i].text);

		switch( array[i].type ) {
		default:
		case 0:
			output(buf);
			break;
		}
	}
}
#endif

/*
* Output version information
*/
static void
gcd_print_version (void)
{
	char	cob_build_stamp[COB_MINI_BUFF];
	char	month[64];
	int 	status, day, year;

	/* Set up build time stamp */
	memset (cob_build_stamp, 0, (size_t)COB_MINI_BUFF);
	memset (month, 0, sizeof(month));
	day = 0;
	year = 0;
	status = sscanf (__DATE__, "%s %d %d", month, &day, &year);
	if (status == 3) {
		snprintf (cob_build_stamp, (size_t)COB_MINI_MAX,
			"%s %02d %04d %s", month, day, year, __TIME__);
	} else {
		snprintf (cob_build_stamp, (size_t)COB_MINI_MAX,
			"%s %s", __DATE__, __TIME__);
	}

	printf ("cobfile (%s) %s.%d\n",
		PACKAGE_NAME, PACKAGE_VERSION, PATCH_LEVEL);
	puts ("Copyright (C) 2021 Free Software Foundation, Inc.");
	puts (_("License GPLv3+: GNU GPL version 3 or later <https://gnu.org/licenses/gpl.html>"));
	puts (_("This is free software; see the source for copying conditions.  There is NO\n"
		"warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE."));
	printf (_("Written by %s"), "Ron Norman");
	putchar ('\n');
	printf (_("Built     %s"), cob_build_stamp);
	putchar ('\n');
	printf (_("Packaged  %s"), COB_TAR_DATE);
	putchar ('\n');
}

/*
 * Display program usage information
*/
static void
gcd_usage (char *prog)
{
	puts (_("Copy/convert data files for GnuCOBOL"));
	putchar ('\n');
	printf (_("usage: %s [options]"), prog);
	putchar ('\n');
	puts (_("Options:"));
#if 0 /* For later use */
	puts (_("  -i  cmdfile    Read commands from this file"));
	puts (_("  -p  progid     Define PROGRAM-ID "));
	puts (_("  -k, -keep      Keep the generated COBOL code"));
#endif
	puts (_("  -D  env=val    Define environment variable"));
	puts (_("  -h, -help      display this help and exit"));
	puts (_("  -V, -version   display version and exit"));
	putchar ('\n');
	printf (_("Report bugs to: %s or\n"
		"use the preferred issue tracker via home page"), "bug-gnucobol@gnu.org");
	putchar ('\n');
	puts (_("GnuCOBOL home page: <http://www.gnu.org/software/gnucobol/>"));
	puts (_("General help using GNU software: <http://www.gnu.org/gethelp/>"));
}

static void
set_option (char *binary, int opt, char *arg)
{
	switch(opt) {
	case 'q':
		be_quiet = 1;
		break;

	case 'k':
		keep_code = 1;
		break;

	case 'i':
		strcpy(cmdfile,arg);
		break;

	case 'p':
		strcpy(progid,arg);
		break;

	case 'D':
		putenv (arg);
		break;

	case '?':
	default:
		printf(_("unknown parameter '%c' for %s"),opt,binary);
		putchar ('\n');
		gcd_usage((char*)"cobfile");
		exit(2);
		break;

	case 'h':
		gcd_usage((char*)"cobfile");
		exit(2);
		break;

	case 'V':
		gcd_print_version ();
		exit(2);
		break;
	}
}

/* Copy the data file */
static int
copyFile (cob_file *fi, cob_file *fo, int skip, int ncopy)
{
	cob_field *fists, *fosts;
	int		recs,written;
	fists = makeField (2);
	fi->file_version = COB_FILE_VERSION;
	fo->file_version = COB_FILE_VERSION;
	if (fi->organization == COB_ORG_RELATIVE) {
		fi->keys[0].field = makeField9 (COB_MAX_DIGITS);
	}
	fi->flag_keycheck = 0;
	fi->flag_auto_type = 1;
	cob_open (fi, COB_OPEN_INPUT, 0, fists);
	if (memcmp(fists->data,"00",2) != 0) {
		printf("Status %.2s opening %s for input\n",fists->data,fi->assign->data);
		return 1;
	}
	fosts = makeField (2);
	if (fo->organization == COB_ORG_RELATIVE) {
		fo->keys[0].field = makeField9 (COB_MAX_DIGITS);
	}

	cob_open (fo, COB_OPEN_OUTPUT, 0, fosts);
	if (memcmp(fosts->data,"00",2) != 0) {
		printf("Status %.2s opening %s for output\n",fosts->data,fo->assign->data);
		return 1;
	}
	/* Copy data */
	written = recs = 0;
	while (1) {
		cob_read_next (fi, fists, COB_READ_NEXT);
		if (fists->data[0] > '0') {
			if (memcmp(fists->data,"10",2) != 0)
				printf("READ status %.2s\n",fists->data);
			break;
		}
		recs++;
		if (skip > 0
		 && recs <= skip)
			continue;
		memcpy (fo->record->data, fi->record->data, fi->record_max);
		cob_write (fo, fo->record, 0, fosts, 0);
		if (fosts->data[0] > '0') {
			printf("WRITE status %.2s\n",fosts->data);
			break;
		}
		written++;
		if (ncopy > 0
		 && written >= ncopy)
			break;
	}
	if (recs == written)
		printf("Copied %d records\n",recs);
	else 
		printf("Read %d records and wrote %d records\n",recs,written);

	/* Close files */
	cob_close (fi, fists, 0, 0);
	dropField (fists);
	cob_close (fo, fosts, 0, 0);
	dropField (fosts);
	return 0;
}

/*
 * M A I N L I N E   Starts here
 */
int
main(
	int		argc,
	char	*argv[])
{
	int		opt,idx,i,k,skip,ncopy;
	FILE	*ref;
	cob_file flin[1], flout[1];
	char	*env, *p;
	char	buf[1024], conffile[256], val[128];
	char	cmd[2560], indef[2560], outdef[2560];

#ifdef	HAVE_SETLOCALE
	setlocale (LC_ALL, "");
#endif
	if(!isatty(0)) {
		batchin = 1;
	} else {
		printf("Commands end with a semicolon;\n");
		printf("To exit enter:   quit;\n");
	}
	/* Process cobfile.conf from current directory */
	ref = fopen("cobfile.conf","r");
	if(ref == NULL) {
		/* Check for cobfile.conf in $HOME directory */
		sprintf(conffile,"%s/cobfile.conf","~");
		ref = fopen(conffile,"r");
	}
	if(ref == NULL) {
		/* Check for cobfile.conf in config directory */
		sprintf(conffile,"%s/cobfile.conf",COB_CONFIG_DIR);
		ref = fopen(conffile,"r");
	}
	if(ref) {
		while (fgets(buf,sizeof(buf),ref) != NULL) {
			k = trim_line (buf);
			if (buf[0] == '-') {	/* Option for cobfile ?*/
				opt = buf[1];
				for (i=2; isspace(buf[i]); i++);
				set_option((char*)"cobfile.conf",opt,&buf[i]);
			}
		}
		fclose(ref);
	}

	idx = 0;
	cob_optind = 1;
	while ((opt = cob_getopt_long_long (argc, argv, short_options,
					  long_options, &idx, 1)) >= 0) {
		set_option(argv[0], opt, cob_optarg);
	}

	cob_extern_init ();
	if ((env = getenv("COB_COPY_DIR")) != NULL) {
		copysearch = strdup (env);
	}
	if ((env = getenv("COBCPY")) != NULL) {
		if (copysearch == NULL) {
			copysearch = strdup (env);
		} else {
			p = malloc (strlen (env) + strlen (copysearch) + 3);
			sprintf(p, "%s%c%s",copysearch,PATHSEP_CHAR,env);
			free (copysearch);
			copysearch = p;
		}
	}
	k = 0;
	cmd[0] = fileindef[0] = indef[0] = outdef[0] = 0;
	memset (flin,  0, sizeof(cob_file));
	memset (flout, 0, sizeof(cob_file));
	flin->record = makeField (65500);
	flout->record = makeField (65500);
	while (getLine (buf)) {
		if (strcasecmp (buf, "quit;") == 0)
			break;
		if (k > 0)
			cmd[k++] = ' ';
		if ((strlen(buf) + k) >= (sizeof(cmd)-1)) {
			printf("Command is too long\n");
			exit (-1);
			break;
		}
		strcpy (&cmd[k], buf);
		k = strlen(cmd);
		if (cmd[k-1] == ';') {
			cmd[k-1] = ' ';
			if (strncasecmp (cmd,"INPUT ",6) == 0) {
				strcpy (indef, cmd+6);
				flin->flag_auto_type = 1;
				parseFile (flin, "INPUT", 256, indef, copyin);
			} else if (strncasecmp (cmd,"OUTPUT ",7) == 0) {
				strcpy (outdef, cmd+7);
				parseFile (flout, "OUTPUT", flin->record_max, outdef, copyout);
			} else if (strncasecmp (cmd,"COPY ",5) == 0) {
				if (indef[0] < ' ') {
					printf("INPUT file is not defined\n");
					continue;
				}
				if (outdef[0] < ' ') {
					printf("OUTPUT file is not defined\n");
					continue;
				}
				skip = ncopy = 0;
				for (k=0; cmd[k] != 0; k++) {
					if (k==0 
					|| cmd[k-1] == ' ') {
						if (matchWord ("SKIP", cmd, val, &k)) {
							skip = atoi (val);
						} else
						if (matchWord ("COPY", cmd, val, &k)) {
							ncopy = atoi (val);
						}
					}
				}
				trim_line (fileindef);
				trim_line (outdef);
				printf("COPY %s\n     '%s'\n",flin->assign->data,fileindef);
				printf("  TO %s\n     '%s'\n",flout->assign->data,outdef);
				copyFile (flin, flout, skip, ncopy);
			} else if (strncasecmp (cmd,"QUIT ",5) == 0) {
				break;
			} else if (strncasecmp (cmd,"EXIT ",5) == 0) {
				break;
			} else {
				printf("Unknown command [%s]\n",cmd);
				gcd_usage((char*)"cobfile");
				printf("To exit enter:  quit;\n");
			}
			k = 0;
		} 
	}

	exit (0);
	return 0;
}