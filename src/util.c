/*
 *  util.c
 *
 *  Copyright (c) 2010-2012 Tuxce <tuxce.net@gmail.com>
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
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <regex.h>
#include <float.h>
#include <time.h>

#include "util.h"
#include "alpm-query.h"
#include "aur.h"
#include "color.h"

#define FORMAT_LOCAL_PKG "lF134"
#define INDENT 4

static alpm_list_t *results = NULL;

/* Results */
typedef struct _results_t
{
	void *ele;
	size_t rel;
	pkgtype_t type;
} results_t;

static results_t *results_new (void *ele, pkgtype_t type)
{
	results_t *r = NULL;
	MALLOC (r, sizeof (results_t));
	r->ele = (type == R_AUR_PKG) ? (void *) aur_pkg_dup ((aurpkg_t *) ele) : ele;
	r->rel = SIZE_MAX;
	r->type = type;
	return r;
}

static results_t *results_free (results_t *r)
{
	if (r) {
		if (r->type == R_AUR_PKG) {
			aur_pkg_free (r->ele);
		}
		free (r);
	}
	return NULL;
}

static const char *results_name (const results_t *r)
{
	if (!r) {
		return NULL;
	}
	switch (r->type) {
		case R_ALPM_PKG:
			return alpm_pkg_get_name ((alpm_pkg_t *) r->ele);
		case R_AUR_PKG:
			return aur_pkg_get_name ((aurpkg_t *) r->ele);
		default:
			return NULL;
	}
}

static time_t results_installdate (const results_t *r)
{
	if (!r || r->type == R_AUR_PKG) {
		return 0;
	}
	time_t idate = 0;
	const char *r_name = results_name (r);
	alpm_pkg_t *pkg = alpm_db_get_pkg (alpm_get_localdb(config.handle), r_name);
	if (pkg) {
		idate = alpm_pkg_get_installdate (pkg);
	}
	return idate;
}

static off_t results_isize (const results_t *r)
{
	if (!r || r->type == R_AUR_PKG) {
		return 0;
	}
	return alpm_pkg_get_isize (r->ele);
}

static int results_votes (const results_t *r)
{
	if (!r || r->type != R_AUR_PKG) {
		return INT_MAX; // put ALPM packages on top
	}
	return aur_pkg_get_votes ((aurpkg_t *) r->ele);
}

static double results_popularity (const results_t *r)
{
	if (!r || r->type != R_AUR_PKG) {
		return DBL_MAX; // put ALPM packages on top
	}
	return aur_pkg_get_popularity ((aurpkg_t *) r->ele);
}

static size_t results_relevance (const results_t *r)
{
	return r ? r->rel : SIZE_MAX;
}

static int results_cmp (const results_t *r1, const results_t *r2)
{
	const char *r1name = results_name (r1);
	const char *r2name = results_name (r2);
	if (!r1name || !r2name) {
		return 0;
	}
	return strcmp (r1name, r2name);
}

static int results_installdate_cmp (const results_t *r1, const results_t *r2)
{
	if (results_installdate (r1) > results_installdate (r2)) return 1;
	if (results_installdate (r2) > results_installdate (r1)) return -1;
	return 0;
}

static int results_isize_cmp (const results_t *r1, const results_t *r2)
{
	if (results_isize (r1) > results_isize (r2)) return 1;
	if (results_isize (r2) > results_isize (r1)) return -1;
	return 0;
}

static int results_votes_cmp (const results_t *r1, const results_t *r2)
{
	return (results_votes (r2) - results_votes (r1));
}

static int results_popularity_cmp (const results_t *r1, const results_t *r2)
{
	if (results_popularity (r1) > results_popularity (r2)) return -1;
	if (results_popularity (r2) > results_popularity (r1)) return 1;
	return 0;
}

static int results_relevance_cmp (const results_t *r1, const results_t *r2)
{
	if (results_relevance (r1) > results_relevance (r2)) return 1;
	if (results_relevance (r2) > results_relevance (r1)) return -1;
	return 0;
}

// https://en.wikibooks.org/wiki/Algorithm_Implementation/Strings/Levenshtein_distance#C
#define MIN3(a, b, c) ((a) < (b) ? ((a) < (c) ? (a) : (c)) : ((b) < (c) ? (b) : (c)))
static size_t levenshtein_distance (const char *s1, const char *s2)
{
	const size_t s1len = strlen (s1);
	const size_t s2len = strlen (s2);
	size_t column[s1len + 1];
	for (size_t x = 1; x <= s1len; x++) {
		column[x] = x;
	}
	for (size_t x = 1; x <= s2len; x++) {
		column[0] = x;
		size_t lastdiag = x - 1;
		for (size_t y = 1; y <= s1len; y++) {
			const size_t olddiag = column[y];
			column[y] = MIN3 (column[y] + 1, column[y-1] + 1, lastdiag + (s1[y-1] == s2[x-1] ? 0 : 1));
			lastdiag = olddiag;
		}
	}
	return column[s1len];
}

void calculate_results_relevance (alpm_list_t *targets)
{
	for (const alpm_list_t *t = targets; t; t = alpm_list_next (t)) {
		const char *target = t->data;
		for (const alpm_list_t *r = results; r; r = alpm_list_next(r)) {
			const char *result = results_name (r->data);
			const size_t lev_dst = levenshtein_distance (target, result);
			if (lev_dst < ((results_t *) r->data)->rel) {
				((results_t *) r->data)->rel = lev_dst;
			}
		}
	}
}

void print_or_add_result (void *pkg, pkgtype_t type)
{
	if (config.sort == 0) {
		print_package ("", pkg, (type == R_ALPM_PKG) ? alpm_pkg_get_str : aur_get_str);
		return;
	}

	results = alpm_list_add (results, results_new (pkg, type));
}

void show_results (void)
{
	if (!results) {
		return;
	}

	alpm_list_fn_cmp fn_cmp = NULL;
	switch (config.sort) {
		case S_NAME:  fn_cmp = (alpm_list_fn_cmp) results_cmp; break;
		case S_VOTE:  fn_cmp = (alpm_list_fn_cmp) results_votes_cmp; break;
		case S_POP:   fn_cmp = (alpm_list_fn_cmp) results_popularity_cmp; break;
		case S_IDATE: fn_cmp = (alpm_list_fn_cmp) results_installdate_cmp; break;
		case S_ISIZE: fn_cmp = (alpm_list_fn_cmp) results_isize_cmp; break;
		case S_REL:   fn_cmp = (alpm_list_fn_cmp) results_relevance_cmp; break;
	}
	if (fn_cmp) {
		results = alpm_list_msort (results, alpm_list_count (results), fn_cmp);
	}

	const alpm_list_nav fn_nav = config.rsort ? alpm_list_previous : alpm_list_next;
	const alpm_list_t *i_first = config.rsort ? alpm_list_last (results) : results;

	for (const alpm_list_t *i = i_first; i; i = fn_nav (i)) {
		const results_t *r = i->data;
		if (r && r->type == R_ALPM_PKG) {
			print_package ("", r->ele, alpm_pkg_get_str);
		} else if (r && r->type == R_AUR_PKG) {
			print_package ("", r->ele, aur_get_str);
		}
	}

	alpm_list_free_inner (results, (alpm_list_fn_free) results_free);
	alpm_list_free (results);
	results = NULL;
}

target_t *target_parse (const char *str)
{
	target_t *ret = NULL;
	char *c, *s = (char *)str;
	MALLOC (ret, sizeof (target_t));
	ret->orig = strdup (str);
	if ((c = strchr (s, '/')) != NULL) {
		/* target include db ("db/pkg*") */
		ret->db = strndup (s, (c-s) / sizeof(char));
		s = ++c;
	} else {
		ret->db = NULL;
	}

	if ((c = strstr (s, "<=")) != NULL) {
		ret->mod = ALPM_DEP_MOD_LE;
		ret->ver = strdup (&(c[2]));
	} else if ((c = strstr (s, ">=")) != NULL) {
		ret->mod = ALPM_DEP_MOD_GE;
		ret->ver = strdup (&(c[2]));
	} else if ((c = strchr (s, '<')) != NULL) {
		ret->mod = ALPM_DEP_MOD_LT;
		ret->ver = strdup (&(c[1]));
	} else if ((c = strchr (s, '>')) != NULL) {
		ret->mod = ALPM_DEP_MOD_GT;
		ret->ver = strdup (&(c[1]));
	} else if ((c = strchr (s, '=')) != NULL) {
		ret->mod = ALPM_DEP_MOD_EQ;
		ret->ver = strdup (&(c[1]));
	} else {
		ret->mod = ALPM_DEP_MOD_ANY;
		ret->ver = NULL;
	}
	ret->name = (c) ? strndup (s, (c-s) / sizeof(char)) : strdup (s);
	return ret;
}

void target_free (target_t *t)
{
	if (!t) {
		return;
	}
	FREE (t->orig);
	FREE (t->db);
	FREE (t->name);
	FREE (t->ver);
	FREE (t);
}

bool target_check_version (const target_t *t, const char *ver)
{
	if (!t || !ver || t->mod == ALPM_DEP_MOD_ANY) {
		return true;
	}
	const int ret = alpm_pkg_vercmp (ver, t->ver);
	switch (t->mod)
	{
		case ALPM_DEP_MOD_LE: return (ret <= 0);
		case ALPM_DEP_MOD_GE: return (ret >= 0);
		case ALPM_DEP_MOD_LT: return (ret < 0);
		case ALPM_DEP_MOD_GT: return (ret > 0);
		case ALPM_DEP_MOD_EQ: return (ret == 0);
		default: return true;
	}
}

bool target_compatible (const target_t *t1, const target_t *t2)
{
	if (!t1 || !t2 || (t2->mod != ALPM_DEP_MOD_EQ && t2->mod != ALPM_DEP_MOD_ANY)) {
		return false;
	}
	if (strcmp (t1->name, t2->name) == 0 &&
			(t1->mod == ALPM_DEP_MOD_ANY || t2->mod == ALPM_DEP_MOD_ANY ||
			target_check_version (t1, t2->ver))) {
		return true;
	}
	return false;
}

int target_name_cmp (const target_t *t1, const char *name)
{
	if (!t1 || !name) {
		return 0;
	}
	return strcmp (t1->name, name);
}

string_t *string_new (void)
{
	string_t *str = NULL;
	MALLOC (str, sizeof (string_t));
	str->size = PATH_MAX;
	MALLOC (str->s, str->size * sizeof (char));
	return str;
}

void string_free (string_t *dest)
{
	if (!dest) {
		return;
	}
	FREE (dest->s);
	FREE (dest);
}

static char *string_free2 (string_t *dest)
{
	if (!dest) {
		return NULL;
	}
	char *s = dest->s;
	FREE (dest);
	return s;
}

string_t *string_ncat (string_t *dest, const char *src, size_t n)
{
	if (!dest || !src || !n) {
		return dest;
	}
	if (dest->size <= dest->used + n) {
		while (dest->size <= dest->used + n) {
			dest->size *= 2;
		}
		REALLOC (dest->s, dest->size * sizeof (char));
	}
	dest->used += n;
	strncat (dest->s, src, n);
	return dest;
}

string_t *string_cat (string_t *dest, const char *src)
{
	if (!src) {
		return dest;
	}
	return string_ncat (dest, src, strlen (src));
}

const char *string_cstr (const string_t *str)
{
	return (const char *) (str ? (str->s ? str->s : "") : "");
}

char *strtrim(char *str)
{
	char *pch = str;

	if (str == NULL || *str == '\0') {
		/* string is empty, so we're done. */
		return str;
	}

	while (isspace(*pch)) {
		pch++;
	}
	if (pch != str) {
		memmove (str, pch, (strlen(pch) + 1));
	}

	/* check if there wasn't anything but whitespace in the string. */
	if (*str == '\0') {
		return str;
	}

	pch = str + (strlen(str) - 1);
	while (isspace(*pch)) {
		pch--;
	}
	*++pch = '\0';

	return str;
}

char *concat_str_list (const alpm_list_t *l)
{
	char *ret = NULL;
	size_t len = 0;
	const size_t sep_len = strlen (config.delimiter);
	int j = 0;

	if (!l) {
		return NULL;
	}

	for (const alpm_list_t *i = l; i; i = alpm_list_next(i)) {
		if (i->data) {
			/* data's len + space for separator */
			len += strlen (i->data) + sep_len;
		}
	}

	if (!len) {
		return NULL;
	}

	len++; /* '\0' at the end */
	CALLOC (ret, len, sizeof (char));
	for (const alpm_list_t *i = l; i; i = alpm_list_next (i)) {
		if (i->data) {
			if (j++ > 0) strcat (ret, config.delimiter);
			strcat (ret, i->data);
		}
	}

	return ret;
}

char *concat_dep_list (const alpm_list_t *deps)
{
	alpm_list_t *deps_str = NULL;
	for (const alpm_list_t *i = deps; i; i = alpm_list_next (i)) {
		deps_str = alpm_list_add (deps_str, alpm_dep_compute_string (i->data));
	}
	char *ret = concat_str_list (deps_str);
	FREELIST (deps_str);
	return ret;
}

char *concat_file_list (const alpm_filelist_t *f)
{
	char *ret = NULL;
	size_t len = 0;
	const size_t sep_len = strlen (config.delimiter);
	int j = 0;

	if (!f) {
		return NULL;
	}

	for (size_t i = 0; i < f->count; i++) {
		const alpm_file_t *file = f->files + i;
		if (file && file->name) {
			/* filename len + space for separator */
			len += strlen (file->name) + sep_len;
		}
	}

	if (!len) {
		return NULL;
	}

	len++; /* '\0' at the end */
	CALLOC (ret, len, sizeof (char));
	for (size_t i = 0; i < f->count; i++) {
		const alpm_file_t *file = f->files + i;
		if (file && file->name) {
			if (j++ > 0) strcat (ret, config.delimiter);
			strcat (ret, file->name);
		}
	}

	return ret;
}

char *concat_backup_list (const alpm_list_t *backups)
{
	alpm_list_t *backups_str = NULL;
	for (const alpm_list_t *i = backups; i; i = alpm_list_next (i)) {
		const alpm_backup_t *backup = i->data;
		if (backup) {
			char *b_str;
			int ret = asprintf (&b_str, "%s\t%s", backup->name, backup->hash);
			if (!b_str || ret < 0) {
				continue;
			}
			backups_str = alpm_list_add (backups_str, b_str);
		}
	}
	char *ret = concat_str_list (backups_str);
	FREELIST (backups_str);
	return ret;
}

void format_str (char *s)
{
	char *c = s;
	while ((c = strchr (c, '\\'))) {
		int mod = 1;
		switch (c[1])
		{
			case '\\': c[0] = '\\'; break;
			case 'e': c[0] = '\033'; break;
			case 'n': c[0] = '\n'; break;
			case 'r': c[0] = '\r'; break;
			case 't': c[0] = '\t'; break;
			default: mod = 0; break;
		}
		if (mod) {
			memcpy (&(c[1]), &(c[2]), (strlen (&(c[2]))+1) * sizeof (char));
		}
		c++;
	}
}

static void print_escape (const char *str)
{
	const char *c = str;
	if (!c) {
		return;
	}
	while (*c != '\0') {
		if (*c == '"') {
			putchar ('\\');
		}
		putchar (*c);
		c++;
	}
}

char *itostr (int i)
{
	char *is;
	int ret = asprintf (&is, "%d", i);
	if (ret < 0) {
		FREE (is);
		return NULL;
	}
	return is;
}

char *ltostr (long i)
{
	char *is;
	int ret = asprintf (&is, "%ld", i);
	if (ret < 0) {
		FREE (is);
		return NULL;
	}
	return is;
}

char *ttostr (time_t t)
{
	char *ts;
	CALLOC (ts, 11, sizeof (char));
	strftime (ts, 11, "%s", localtime (&t));
	return ts;
}

/* Helper function for strreplace */
static void _strnadd (char **str, const char *append, size_t count)
{
	if (!str) {
		return;
	}

	if (*str) {
		REALLOC (*str, strlen (*str) + count + 1);
	} else {
		CALLOC (*str, sizeof (char), count + 1);
	}

	strncat (*str, append, count);
}

/* Replace all occurances of 'needle' with 'replace' in 'str', returning
 * a new string (must be free'd) */
char *strreplace (const char *str, const char *needle, const char *replace)
{
	const char *p = str, *q = str;
	char *newstr = NULL;
	const size_t needlesz = strlen (needle), replacesz = strlen (replace);

	while (1) {
		q = strstr (p, needle);
		if (!q) { /* not found */
			if (*p) {
				/* add the rest of 'p' */
				_strnadd (&newstr, p, strlen (p));
			}
			break;
		} else { /* found match */
			if (q > p) {
				/* add chars between this occurance and last occurance, if any */
				_strnadd (&newstr, p, q - p);
			}
			_strnadd (&newstr, replace, replacesz);
			p = q + needlesz;
		}
	}

	return newstr;
}

/** Parse the basename of a program from a path.
* @param path path to parse basename from
*
* @return everything following the final '/'
*/
const char *mbasename (const char *path)
{
	const char *last = strrchr (path, '/');
	if (last) {
		return last + 1;
	}
	return path;
}

static int getcols(void)
{
#ifdef TIOCGSIZE
	struct ttysize win;
	if (ioctl(1, TIOCGSIZE, &win) == 0) {
		return win.ts_cols;
	}
#elif defined(TIOCGWINSZ)
	struct winsize win;
	if (ioctl(1, TIOCGWINSZ, &win) == 0) {
		return win.ws_col;
	}
#endif
	return 0;
}

static void indent (const char *str)
{
	if (!str) {
		return;
	}
	const int cols = getcols();
	if (!cols) {
		fprintf (stdout, "%*s%s\n", INDENT, "", str);
		return;
	}
	const char *c = str;
	const char *c1 = NULL;
	int cur_col = INDENT;
	fprintf (stdout, "%*s", INDENT, "");
	while ((c1 = strchr (c, ' ')) != NULL) {
		const int len = c1 - c;
		cur_col += len + 1;
		if (cur_col >= cols) {
			fprintf (stdout, "\n%*s%.*s ", INDENT, "", len, c);
			cur_col = INDENT + len + 1;
		} else {
			fprintf (stdout, "%.*s ", len, c);
		}
		c = &c1[1];
	}
	cur_col += strlen (c);
	if (cur_col >= cols && c != str) {
		fprintf (stdout, "\n%*s%s\n", INDENT, "", c);
	} else {
		fprintf (stdout, "%s\n", c);
	}
}

/* Helper functions for color_print_package() */

static const char *color_print_repo (void *p, printpkgfn f)
{
	const char *info = (config.aur_foreign) ? f(p, 'r') : f(p, 's');
	if (info) {
		if (config.get_res) {
			dprintf (FD_RES, "%s/", info);
		}
		fprintf (stdout, "%s%s/%s", color_repo (info), info, color(C_NO));
	}
	info = f(p, 'n');
	if (config.get_res) {
		dprintf (FD_RES, "%s\n", info);
	}
	fprintf (stdout, "%s%s%s ", color(C_PKG), info, color(C_NO));
	return info;
}

static const char *color_print_aur_version (void *p, printpkgfn f, const char *i, const char *lver,
											 const char *ver)
{
	const char *info = NULL, *lver_color = NULL;
	if (!i) {
		lver_color = color(C_ORPHAN);
	} else {
		info = f(p, 'o');
		if (info && info[0] == '1') {
			lver_color = color(C_OD);
		}
	}
	fprintf (stdout, "%s%s%s", (lver_color) ? lver_color : color(C_VER), lver, color(C_NO));
	if (alpm_pkg_vercmp (ver, lver) > 0) {
		fprintf (stdout, " ( aur: %s )", ver);
	}
	fprintf (stdout, "\n");
	return info;
}

static void color_print_size (void *p, printpkgfn f)
{
	const char *info = f(p, 'r');
	if (info && strcmp (info, "aur") != 0) {
		fprintf (stdout, " [%.2f M]", (double) get_size_pkg (p) / (1024.0 * 1024));
	}
}

static void color_print_groups (void *p, printpkgfn f)
{
	const char *info = f(p, 'g');
	if (info) {
		fprintf (stdout, " %s(%s)%s", color(C_GRP), info, color(C_NO));
	}
}

static void color_print_install_info (void *p, printpkgfn f, const char *lver, const char *ver)
{
	if (lver) {
		const char *info = f(p, 'r');
		if (info && strcmp (info, "local") != 0) {
			fprintf (stdout, " %s[%s", color(C_INSTALLED), _("installed"));
			if (strcmp (ver, lver) != 0) {
				fprintf (stdout, ": %s%s%s%s", color(C_LVER), lver, color(C_NO), color(C_INSTALLED));
			}
			fprintf (stdout, "]%s", color(C_NO));
		}
	}
}

static void color_print_aur_status (void *p, printpkgfn f)
{
	const char *info = f(p, 'o');
	if (info && info[0] != '0') {
		fprintf (stdout, " %s(%s)%s", color(C_OD), _("Out of Date"), color(C_NO));
	}
	info = f(p, 'w');
	if (info) {
		fprintf (stdout, " %s(%s)%s", color(C_VOTES), info, color(C_NO));
	}
	info = f(p, 'p');
	if (info) {
		fprintf (stdout, " %s(%s)%s", color(C_POPUL), info, color(C_NO));
	}
}

static void color_print_package (void *p, printpkgfn f)
{
	static int number = 0;
	const bool aur = (f == aur_get_str);
	const bool grp = (f == alpm_grp_get_str);

	/* Numbering list */
	if (config.numbering) {
		fprintf (stdout, "%s%d%s ", color (C_NB), ++number, color (C_NO));
	}

	/* repo/name */
	const char *info = color_print_repo (p, f);

	if (grp) {
		/* no more output for groups */
		fprintf (stdout, "\n");
		return;
	}

	/* Version
	 * different colors:
	 *   C_ORPHAN if package exists in AUR and is orphaned
	 *   C_OD if package exists and is out of date
	 *   C_VER otherwise
	 */
	const char *lver = alpm_local_pkg_get_str (info, 'l');
	info = f(p, (config.aur_upgrades || config.filter & F_UPGRADES) ? 'V' : 'v');
	char *ver = STRDUP (info);
	info = (aur) ? f(p, 'm') : NULL;
	if (config.aur_foreign) {
		/* Compare foreign package with AUR */
		if (aur) {
			info = color_print_aur_version (p, f, info, lver, ver);
		} else {
			fprintf (stdout, " %s%s%s\n", color(C_VER), lver, color(C_NO));
		}
		FREE (ver);
		return;
	}

	if (aur && !info) {
		fprintf (stdout, "%s", color(C_ORPHAN));
	} else {
		fprintf (stdout, "%s", color(C_VER));
	}
	if (config.filter & F_UPGRADES) {
		fprintf (stdout, "%s%s -> %s%s%s", lver, color (C_NO), color(C_VER), ver, color (C_NO));
	} else {
		fprintf (stdout, "%s%s", ver, color (C_NO));
	}

	/* show size */
	if (config.show_size) {
		color_print_size (p, f);
	}

	if (config.aur_upgrades || config.filter & F_UPGRADES) {
		fprintf (stdout, "\n");
		FREE (ver);
		return;
	}

	/* show groups */
	color_print_groups (p, f);

	/* show install information */
	color_print_install_info (p, f, lver, ver);

	/* ver no more needed */
	FREE (ver);

	/* Out of date status & votes */
	if (aur) {
		color_print_aur_status (p, f);
	}

	/* Nothing more to display */
	fprintf (stdout, "\n");

	/* Description
	 * if -Q or -Sl or -Sg <target>, don't display description
	 */
	if (config.op != OP_SEARCH && config.op != OP_LIST_REPO_S) {
		return;
	}
	fprintf (stdout, "%s", color(C_DSC));
	indent (f(p, 'd'));
	fprintf (stdout, "%s", color(C_NO));
}

void print_package (const char *target, void *pkg, printpkgfn f)
{
	if (config.quiet || !target || !pkg || !f) {
		return;
	}

	if (!config.custom_out) {
		color_print_package (pkg, f);
		return;
	}

	char *s = pkg_to_str (target, pkg, f, config.format_out);
	if (!s) {
		return;
	}

	if (config.escape) {
		print_escape (s);
	} else {
		printf ("%s\n", s);
	}
	free (s);
	fflush (NULL);
}

char *pkg_to_str (const char *target, void *pkg, printpkgfn f, const char *format)
{
	if (!format) return NULL;
	const char *c;
	string_t *ret = string_new();
	const char *ptr = format;
	const char *end = &(format[strlen(format)]);
	while ((c = strchr (ptr, '%'))) {
		if (&(c[1]) == end) {
			break;
		}
		if (c[1] == '%' ) {
			ret = string_ncat (ret, ptr, (c-ptr));
			ret = string_cat (ret, "%%");
		} else {
			const char *info = NULL;
			if (strchr (FORMAT_LOCAL_PKG, c[1])) {
				info = alpm_local_pkg_get_str (f(pkg, 'n'), c[1]);
			} else if (c[1] == 't') {
				info = target;
			} else {
				info = f (pkg, c[1]);
			}
			if (c != ptr) {
				ret = string_ncat (ret, ptr, (c-ptr));
			}
			if (info) {
				ret = string_cat (ret, info);
			} else {
				ret = string_cat (ret, "-");
			}
		}
		ptr = &(c[2]);
	}
	if (ptr != end) {
		ret = string_ncat (ret, ptr, (end - ptr));
	}
	return string_free2 (ret);
}

target_arg_t *target_arg_init (ta_dup_fn dup_fn, alpm_list_fn_cmp cmp_fn,
								alpm_list_fn_free free_fn)
{
	target_arg_t *t;
	MALLOC (t, sizeof (target_arg_t));
	t->args = NULL;
	t->items = NULL;
	t->dup_fn = dup_fn;
	t->cmp_fn = cmp_fn;
	t->free_fn = free_fn;
	return t;
}

bool target_arg_add (target_arg_t *t, const char *s, void *item)
{
	bool ret = true;
	if (t && config.just_one) {
		if ((t->cmp_fn && alpm_list_find (t->items, item, t->cmp_fn)) ||
				(!t->cmp_fn && alpm_list_find_ptr (t->items, item))) {
			ret = false;
		} else if (t->dup_fn) {
			t->items = alpm_list_add (t->items, t->dup_fn (item));
		} else {
			t->items = alpm_list_add (t->items, item);
		}
		t->args = alpm_list_add (t->args, (void *) s);
	}
	return ret;
}

static void target_arg_free (target_arg_t *t)
{
	if (t) {
		if (t->free_fn) {
			alpm_list_free_inner (t->items, (alpm_list_fn_free) t->free_fn);
		}
		alpm_list_free (t->items);
		alpm_list_free (t->args);
		FREE (t);
	}
}

alpm_list_t *target_arg_clear (target_arg_t *t, alpm_list_t *targets)
{
	if (t && targets && config.just_one) {
		for (alpm_list_t *i = t->args; i; i = alpm_list_next (i)) {
			char *data = NULL;
			targets = alpm_list_remove_str (targets, i->data, &data);
			if (data) {
				free (data);
			}
		}
	}
	return targets;
}

alpm_list_t *target_arg_close (target_arg_t *t, alpm_list_t *targets)
{
	targets = target_arg_clear (t, targets);
	target_arg_free (t);
	return targets;
}

bool does_name_contain_targets (const alpm_list_t *targets, const char *name, bool use_regex)
{
	if (!targets || !name) {
		return false;
	}

	for (const alpm_list_t *t = targets; t; t = alpm_list_next (t)) {
		const char *target = (const char *)(t->data);
		if (!target) {
			continue;
		}

		if (use_regex) {
			regex_t reg;
			if (regcomp (&reg, target, REG_EXTENDED | REG_NOSUB | REG_ICASE | REG_NEWLINE) != 0) {
				return false;
			}
			if (regexec (&reg, name, 0, 0, 0) != 0 && strstr (name, target) == NULL) {
				regfree (&reg);
				return false;
			}
			regfree (&reg);
		} else if (strcasestr (name, target) == NULL) {
			return false;
		}
	}

	return true;
}

/* vim: set ts=4 sw=4 noet: */
