/*
 *  LibXDiff by Davide Libenzi ( File Differential Library )
 *  Copyright (C) 2003	Davide Libenzi
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  Davide Libenzi <davidel@xmailserver.org>
 *
 */

#include "xinclude.h"



#define XDL_MAX_COST_MIN 256
#define XDL_HEUR_MIN_COST 256
#define XDL_LINE_MAX (long)((1UL << (CHAR_BIT * sizeof(long) - 1)) - 1)
#define XDL_SNAKE_CNT 20
#define XDL_K_HEUR 4



typedef struct s_xdpsplit {
	long i1, i2;
	int min_lo, min_hi;
} xdpsplit_t;




static long xdl_split(unsigned long const *ha1, long off1, long lim1,
		      unsigned long const *ha2, long off2, long lim2,
		      long *kvdf, long *kvdb, int need_min, xdpsplit_t *spl,
		      xdalgoenv_t *xenv);
static xdchange_t *xdl_add_change(xdchange_t *xscr, long i1, long i2, long chg1, long chg2);





/*
 * See "An O(ND) Difference Algorithm and its Variations", by Eugene Myers.
 * Basically considers a "box" (off1, off2, lim1, lim2) and scan from both
 * the forward diagonal starting from (off1, off2) and the backward diagonal
 * starting from (lim1, lim2). If the K values on the same diagonal crosses
 * returns the furthest point of reach. We might end up having to expensive
 * cases using this algorithm is full, so a little bit of heuristic is needed
 * to cut the search and to return a suboptimal point.
 */
static long xdl_split(unsigned long const *ha1, long off1, long lim1,
		      unsigned long const *ha2, long off2, long lim2,
		      long *kvdf, long *kvdb, int need_min, xdpsplit_t *spl,
		      xdalgoenv_t *xenv) {
	long dmin = off1 - lim2, dmax = lim1 - off2;
	long fmid = off1 - off2, bmid = lim1 - lim2;
	long odd = (fmid - bmid) & 1;
	long fmin = fmid, fmax = fmid;
	long bmin = bmid, bmax = bmid;
	long ec, d, i1, i2, prev1, best, dd, v, k;

	/*
	 * Set initial diagonal values for both forward and backward path.
	 */
	kvdf[fmid] = off1;
	kvdb[bmid] = lim1;

	for (ec = 1;; ec++) {
		int got_snake = 0;

		/*
		 * We need to extent the diagonal "domain" by one. If the next
		 * values exits the box boundaries we need to change it in the
		 * opposite direction because (max - min) must be a power of two.
		 * Also we initialize the external K value to -1 so that we can
		 * avoid extra conditions check inside the core loop.
		 */
		if (fmin > dmin)
			kvdf[--fmin - 1] = -1;
		else
			++fmin;
		if (fmax < dmax)
			kvdf[++fmax + 1] = -1;
		else
			--fmax;

		for (d = fmax; d >= fmin; d -= 2) {
			if (kvdf[d - 1] >= kvdf[d + 1])
				i1 = kvdf[d - 1] + 1;
			else
				i1 = kvdf[d + 1];
			prev1 = i1;
			i2 = i1 - d;
			for (; i1 < lim1 && i2 < lim2 && ha1[i1] == ha2[i2]; i1++, i2++);
			if (i1 - prev1 > xenv->snake_cnt)
				got_snake = 1;
			kvdf[d] = i1;
			if (odd && bmin <= d && d <= bmax && kvdb[d] <= i1) {
				spl->i1 = i1;
				spl->i2 = i2;
				spl->min_lo = spl->min_hi = 1;
				return ec;
			}
		}

		/*
		 * We need to extent the diagonal "domain" by one. If the next
		 * values exits the box boundaries we need to change it in the
		 * opposite direction because (max - min) must be a power of two.
		 * Also we initialize the external K value to -1 so that we can
		 * avoid extra conditions check inside the core loop.
		 */
		if (bmin > dmin)
			kvdb[--bmin - 1] = XDL_LINE_MAX;
		else
			++bmin;
		if (bmax < dmax)
			kvdb[++bmax + 1] = XDL_LINE_MAX;
		else
			--bmax;

		for (d = bmax; d >= bmin; d -= 2) {
			if (kvdb[d - 1] < kvdb[d + 1])
				i1 = kvdb[d - 1];
			else
				i1 = kvdb[d + 1] - 1;
			prev1 = i1;
			i2 = i1 - d;
			for (; i1 > off1 && i2 > off2 && ha1[i1 - 1] == ha2[i2 - 1]; i1--, i2--);
			if (prev1 - i1 > xenv->snake_cnt)
				got_snake = 1;
			kvdb[d] = i1;
			if (!odd && fmin <= d && d <= fmax && i1 <= kvdf[d]) {
				spl->i1 = i1;
				spl->i2 = i2;
				spl->min_lo = spl->min_hi = 1;
				return ec;
			}
		}

		if (need_min)
			continue;

		/*
		 * If the edit cost is above the heuristic trigger and if
		 * we got a good snake, we sample current diagonals to see
		 * if some of the, have reached an "interesting" path. Our
		 * measure is a function of the distance from the diagonal
		 * corner (i1 + i2) penalized with the distance from the
		 * mid diagonal itself. If this value is above the current
		 * edit cost times a magic factor (XDL_K_HEUR) we consider
		 * it interesting.
		 */
		if (got_snake && ec > xenv->heur_min) {
			for (best = 0, d = fmax; d >= fmin; d -= 2) {
				dd = d > fmid ? d - fmid: fmid - d;
				i1 = kvdf[d];
				i2 = i1 - d;
				v = (i1 - off1) + (i2 - off2) - dd;

				if (v > XDL_K_HEUR * ec && v > best &&
				    off1 + xenv->snake_cnt <= i1 && i1 < lim1 &&
				    off2 + xenv->snake_cnt <= i2 && i2 < lim2) {
					for (k = 1; ha1[i1 - k] == ha2[i2 - k]; k++)
						if (k == xenv->snake_cnt) {
							best = v;
							spl->i1 = i1;
							spl->i2 = i2;
							break;
						}
				}
			}
			if (best > 0) {
				spl->min_lo = 1;
				spl->min_hi = 0;
				return ec;
			}

			for (best = 0, d = bmax; d >= bmin; d -= 2) {
				dd = d > bmid ? d - bmid: bmid - d;
				i1 = kvdb[d];
				i2 = i1 - d;
				v = (lim1 - i1) + (lim2 - i2) - dd;

				if (v > XDL_K_HEUR * ec && v > best &&
				    off1 < i1 && i1 <= lim1 - xenv->snake_cnt &&
				    off2 < i2 && i2 <= lim2 - xenv->snake_cnt) {
					for (k = 0; ha1[i1 + k] == ha2[i2 + k]; k++)
						if (k == xenv->snake_cnt - 1) {
							best = v;
							spl->i1 = i1;
							spl->i2 = i2;
							break;
						}
				}
			}
			if (best > 0) {
				spl->min_lo = 0;
				spl->min_hi = 1;
				return ec;
			}
		}

		/*
		 * Enough is enough. We spent too much time here and now we collect
		 * the furthest reaching path using the (i1 + i2) measure.
		 */
		if (ec >= xenv->mxcost) {
			long fbest, fbest1, bbest, bbest1;

			fbest = fbest1 = -1;
			for (d = fmax; d >= fmin; d -= 2) {
				i1 = XDL_MIN(kvdf[d], lim1);
				i2 = i1 - d;
				if (lim2 < i2)
					i1 = lim2 + d, i2 = lim2;
				if (fbest < i1 + i2) {
					fbest = i1 + i2;
					fbest1 = i1;
				}
			}

			bbest = bbest1 = XDL_LINE_MAX;
			for (d = bmax; d >= bmin; d -= 2) {
				i1 = XDL_MAX(off1, kvdb[d]);
				i2 = i1 - d;
				if (i2 < off2)
					i1 = off2 + d, i2 = off2;
				if (i1 + i2 < bbest) {
					bbest = i1 + i2;
					bbest1 = i1;
				}
			}

			if ((lim1 + lim2) - bbest < fbest - (off1 + off2)) {
				spl->i1 = fbest1;
				spl->i2 = fbest - fbest1;
				spl->min_lo = 1;
				spl->min_hi = 0;
			} else {
				spl->i1 = bbest1;
				spl->i2 = bbest - bbest1;
				spl->min_lo = 0;
				spl->min_hi = 1;
			}
			return ec;
		}
	}
}


/*
 * Rule: "Divide et Impera". Recursively split the box in sub-boxes by calling
 * the box splitting function. Note that the real job (marking changed lines)
 * is done in the two boundary reaching checks.
 */
int xdl_recs_cmp(diffdata_t *dd1, long off1, long lim1,
		 diffdata_t *dd2, long off2, long lim2,
		 long *kvdf, long *kvdb, int need_min, xdalgoenv_t *xenv) {
	unsigned long const *ha1 = dd1->ha, *ha2 = dd2->ha;

	/*
	 * Shrink the box by walking through each diagonal snake (SW and NE).
	 */
	for (; off1 < lim1 && off2 < lim2 && ha1[off1] == ha2[off2]; off1++, off2++);
	for (; off1 < lim1 && off2 < lim2 && ha1[lim1 - 1] == ha2[lim2 - 1]; lim1--, lim2--);

	/*
	 * If one dimension is empty, then all records on the other one must
	 * be obviously changed.
	 */
	if (off1 == lim1) {
		char *rchg2 = dd2->rchg;
		long *rindex2 = dd2->rindex;

		for (; off2 < lim2; off2++)
			rchg2[rindex2[off2]] = 1;
	} else if (off2 == lim2) {
		char *rchg1 = dd1->rchg;
		long *rindex1 = dd1->rindex;

		for (; off1 < lim1; off1++)
			rchg1[rindex1[off1]] = 1;
	} else {
		xdpsplit_t spl;
		spl.i1 = spl.i2 = 0;

		/*
		 * Divide ...
		 */
		if (xdl_split(ha1, off1, lim1, ha2, off2, lim2, kvdf, kvdb,
			      need_min, &spl, xenv) < 0) {

			return -1;
		}

		/*
		 * ... et Impera.
		 */
		if (xdl_recs_cmp(dd1, off1, spl.i1, dd2, off2, spl.i2,
				 kvdf, kvdb, spl.min_lo, xenv) < 0 ||
		    xdl_recs_cmp(dd1, spl.i1, lim1, dd2, spl.i2, lim2,
				 kvdf, kvdb, spl.min_hi, xenv) < 0) {

			return -1;
		}
	}

	return 0;
}


int xdl_do_diff(mmfile_t *mf1, mmfile_t *mf2, xpparam_t const *xpp,
		xdfenv_t *xe) {
	long ndiags;
	long *kvd, *kvdf, *kvdb;
	xdalgoenv_t xenv;
	diffdata_t dd1, dd2;

	if (XDF_DIFF_ALG(xpp->flags) == XDF_PATIENCE_DIFF)
		return xdl_do_patience_diff(mf1, mf2, xpp, xe);

	if (XDF_DIFF_ALG(xpp->flags) == XDF_HISTOGRAM_DIFF)
		return xdl_do_histogram_diff(mf1, mf2, xpp, xe);

	if (xdl_prepare_env(mf1, mf2, xpp, xe) < 0) {

		return -1;
	}

	/*
	 * Allocate and setup K vectors to be used by the differential algorithm.
	 * One is to store the forward path and one to store the backward path.
	 */
	ndiags = xe->xdf1.nreff + xe->xdf2.nreff + 3;
	if (!(kvd = (long *) xdl_malloc((2 * ndiags + 2) * sizeof(long)))) {

		xdl_free_env(xe);
		return -1;
	}
	kvdf = kvd;
	kvdb = kvdf + ndiags;
	kvdf += xe->xdf2.nreff + 1;
	kvdb += xe->xdf2.nreff + 1;

	xenv.mxcost = xdl_bogosqrt(ndiags);
	if (xenv.mxcost < XDL_MAX_COST_MIN)
		xenv.mxcost = XDL_MAX_COST_MIN;
	xenv.snake_cnt = XDL_SNAKE_CNT;
	xenv.heur_min = XDL_HEUR_MIN_COST;

	dd1.nrec = xe->xdf1.nreff;
	dd1.ha = xe->xdf1.ha;
	dd1.rchg = xe->xdf1.rchg;
	dd1.rindex = xe->xdf1.rindex;
	dd2.nrec = xe->xdf2.nreff;
	dd2.ha = xe->xdf2.ha;
	dd2.rchg = xe->xdf2.rchg;
	dd2.rindex = xe->xdf2.rindex;

	if (xdl_recs_cmp(&dd1, 0, dd1.nrec, &dd2, 0, dd2.nrec,
			 kvdf, kvdb, (xpp->flags & XDF_NEED_MINIMAL) != 0, &xenv) < 0) {

		xdl_free(kvd);
		xdl_free_env(xe);
		return -1;
	}

	xdl_free(kvd);

	return 0;
}


static xdchange_t *xdl_add_change(xdchange_t *xscr, long i1, long i2, long chg1, long chg2) {
	xdchange_t *xch;

	if (!(xch = (xdchange_t *) xdl_malloc(sizeof(xdchange_t))))
		return NULL;

	xch->next = xscr;
	xch->i1 = i1;
	xch->i2 = i2;
	xch->chg1 = chg1;
	xch->chg2 = chg2;
	xch->ignore = 0;

	return xch;
}


static int is_blank_line(xrecord_t *rec, long flags)
{
	return xdl_blankline(rec->ptr, rec->size, flags);
}

static int recs_match(xrecord_t **recs, long ixs, long ix, long flags)
{
	return (recs[ixs]->ha == recs[ix]->ha &&
		xdl_recmatch(recs[ixs]->ptr, recs[ixs]->size,
			     recs[ix]->ptr, recs[ix]->size,
			     flags));
}

/*
 * If a line is indented more than this, get_indent() just returns this value.
 * This avoids having to do absurd amounts of work for data that are not
 * human-readable text, and also ensures that the output of get_indent fits within
 * an int.
 */
#define MAX_INDENT 200

/*
 * Return the amount of indentation of the specified line, treating TAB as 8
 * columns. Return -1 if line is empty or contains only whitespace. Clamp the
 * output value at MAX_INDENT.
 */
static int get_indent(xrecord_t *rec)
{
	long i;
	int ret = 0;

	for (i = 0; i < rec->size; i++) {
		char c = rec->ptr[i];

		if (!XDL_ISSPACE(c))
			return ret;
		else if (c == ' ')
			ret += 1;
		else if (c == '\t')
			ret += 8 - ret % 8;
		/* ignore other whitespace characters */

		if (ret >= MAX_INDENT)
			return MAX_INDENT;
	}
	/*
	 * We have reached the end of the line without finding any non-space
	 * characters; i.e., the whole line consists of trailing spaces, which we
	 * are not interested in.
	 */
	return -1;
}

/*
 * If more than this number of consecutive blank rows are found, just return this
 * value. This avoids requiring O(N^2) work for pathological cases, and also
 * ensures that the output of score_split fits in an int.
 */
#define MAX_BLANKS 20

/* Characteristics measured about a hypothetical split position. */
struct split_measurement {
	/*
	 * Is the split at the end of the file (aside from any blank lines)?
	 */
	int end_of_file;

	/*
	 * How much is the line immediately following the split indented (or -1 if
	 * the line is blank):
	 */
	int indent;

	/*
	 * How many consecutive lines above the split are blank?
	 */
	int pre_blank;

	/*
	 * How much is the nearest non-blank line above the split indented (or -1
	 * if there is no such line)?
	 */
	int pre_indent;

	/*
	 * How many lines after the line following the split are blank?
	 */
	int post_blank;

	/*
	 * How much is the nearest non-blank line after the line following the
	 * split indented (or -1 if there is no such line)?
	 */
	int post_indent;
};

/*
 * Fill m with information about a hypothetical split of xdf above line split.
 */
void measure_split(const xdfile_t *xdf, long split, struct split_measurement *m)
{
	long i;

	if (split >= xdf->nrec) {
		m->end_of_file = 1;
		m->indent = -1;
	} else {
		m->end_of_file = 0;
		m->indent = get_indent(xdf->recs[split]);
	}

	m->pre_blank = 0;
	for (i = split - 1; i >= 0; i--) {
		m->pre_indent = get_indent(xdf->recs[i]);
		if (m->pre_indent != -1)
			break;
		m->pre_blank += 1;
		if (m->pre_blank == MAX_BLANKS) {
			m->pre_indent = 0;
			break;
		}
	}

	m->post_blank = 0;
	for (i = split + 1; i < xdf->nrec; i++) {
		m->post_indent = get_indent(xdf->recs[i]);
		if (m->post_indent != -1)
			break;
		m->post_blank += 1;
		if (m->post_blank == MAX_BLANKS) {
			m->post_indent = 0;
			break;
		}
	}
}

#define START_OF_FILE_BONUS 9
#define END_OF_FILE_BONUS 46
#define TOTAL_BLANK_WEIGHT 4
#define PRE_BLANK_WEIGHT 16
#define RELATIVE_INDENT_BONUS -1
#define RELATIVE_INDENT_HAS_BLANK_BONUS 15
#define RELATIVE_OUTDENT_BONUS -19
#define RELATIVE_OUTDENT_HAS_BLANK_BONUS 2
#define RELATIVE_DEDENT_BONUS -63
#define RELATIVE_DEDENT_HAS_BLANK_BONUS 50

/*
 * Compute a badness score for the hypothetical split whose measurements are
 * stored in m. The weight factors were determined empirically using the tools and
 * corpus described in
 *
 *     https://github.com/mhagger/diff-slider-tools
 *
 * Also see that project if you want to improve the weights based on, for example,
 * a larger or more diverse corpus.
 */
int score_split(const struct split_measurement *m)
{
	/*
	 * A place to accumulate bonus factors (positive makes this index more
	 * favored):
	 */
	int bonus = 0, score, total_blanks, indent, any_blanks;

	if (m->pre_indent == -1 && m->pre_blank == 0)
		bonus += START_OF_FILE_BONUS;

	if (m->end_of_file)
		bonus += END_OF_FILE_BONUS;

	total_blanks = m->pre_blank;
	if (m->indent == -1)
		total_blanks += 1 + m->post_blank;

	/* Bonuses based on the location of blank lines: */
	bonus += TOTAL_BLANK_WEIGHT * total_blanks;
	bonus += PRE_BLANK_WEIGHT * m->pre_blank;

	if (m->indent != -1)
		indent = m->indent;
	else
		indent = m->post_indent;

	any_blanks = (total_blanks != 0);

	if (indent == -1) {
		score = 0;
	} else if (m->pre_indent == -1) {
		score = indent;
	} else if (indent > m->pre_indent) {
		/*
		 * The line is indented more than its predecessor. Score it based
		 * on the larger indent:
		 */
		score = indent;
		bonus += RELATIVE_INDENT_BONUS;
		bonus += RELATIVE_INDENT_HAS_BLANK_BONUS * any_blanks;
	} else if (indent < m->pre_indent) {
		/*
		 * The line is indented less than its predecessor. It could be
		 * that this line is the start of a new block (e.g., of an "else"
		 * block, or of a block without a block terminator) or it could be
		 * the end of the previous block.
		 */
		if (m->post_indent == -1 || indent >= m->post_indent) {
			/*
			 * That was probably the end of a block. Score based on
			 * the line's own indent:
			 */
			score = indent;
			bonus += RELATIVE_DEDENT_BONUS;
			bonus += RELATIVE_DEDENT_HAS_BLANK_BONUS * any_blanks;
		} else {
			/*
			 * The following line is indented more. So it is likely
			 * that this line is the start of a block. It's a pretty
			 * good place to split, so score it based on its own
			 * indent:
			 */
			score = indent;
			bonus += RELATIVE_OUTDENT_BONUS;
			bonus += RELATIVE_OUTDENT_HAS_BLANK_BONUS * any_blanks;
		}
	} else {
		/*
		 * The line has the same indentation level as its predecessor. We
		 * score it based on its own indent:
		 */
		score = indent;
	}

	return 10 * score - bonus;
}

int xdl_change_compact(xdfile_t *xdf, xdfile_t *xdfo, long flags) {
	long start, end, earliest_end, end_matching_other;
	long io, groupsize, nrec = xdf->nrec;
	char *rchg = xdf->rchg, *rchgo = xdfo->rchg;
	unsigned int blank_lines;
	xrecord_t **recs = xdf->recs;

	/*
	 * This is the same of what GNU diff does. Move back and forward
	 * change groups for a consistent and pretty diff output. This also
	 * helps in finding joinable change groups and reduce the diff size.
	 */
	end = io = 0;
	while (1) {
		/*
		 * Find the first changed line in the to-be-compacted file.
		 * We need to keep track of both indexes, so if we find a
		 * changed lines group on the other file, while scanning the
		 * to-be-compacted file, we need to skip it properly. Note
		 * that loops that are testing for changed lines on rchg* do
		 * not need index bounding since the array is prepared with
		 * a zero at position -1 and N.
		 */
		for (start = end; start < nrec && !rchg[start]; start++) {
			/* skip over any changed lines in the other file... */
			while (rchgo[io])
				io++;

			/* ...plus one non-changed line. */
			io++;
		}
		if (start == nrec)
			break;

		/*
		 * That's the start of a changed-group in the to-be-compacted
		 * file. Now find its end.
		 */
		end = start + 1;
		while (rchg[end])
			end++;

		while (rchgo[io])
		       io++;

		/*
		 * Now shift the change up and then down as far as possible in
		 * each direction. If it bumps into any other changes, merge them.
		 * If there are any changes in the other file that this change
		 * could line up with, set end_matching_other to the end position
		 * of this change that would leave them aligned.
		 */
		do {
			groupsize = end - start;

			/*
			 * Are there any blank lines that could appear as the last
			 * line of this group?
			 */
			blank_lines = 0;

			/*
			 * While the line before the current change group is equal
			 * to the last line of the current change group, shift the
			 * group backward.
			 */
			while (start > 0 && recs_match(recs, start - 1, end - 1, flags)) {
				rchg[--start] = 1;
				rchg[--end] = 0;

				/*
				 * This change might have joined two change groups.
				 * If so, move the start index to the beginning of
				 * the combined group:
				 */
				while (rchg[start - 1])
					start--;

				/*
				 * Move the other file index past a non-changed
				 * line...
				 */
				io--;

				/* ...and also past any changed lines: */
				while (rchgo[io])
					io--;
			}

			if (rchgo[io - 1]) {
				/*
				 * This change is matched to a group of changes in
				 * the other file. Record the end-of-group
				 * position:
				 */
				end_matching_other = end;
			} else {
				/*
				 * Otherwise, set a value to signify that there
				 * are no matched changes in the other file:
				 */
				end_matching_other = -1;
			}

			earliest_end = end;

			/*
			 * Now shift the group forward as long as the first line
			 * of the current change group is equal to the line after
			 * the current change group.
			 */
			while (end < nrec && recs_match(recs, start, end, flags)) {
				blank_lines += is_blank_line(recs[end], flags);

				rchg[start++] = 0;
				rchg[end++] = 1;

				/*
				 * This change might have joined two change
				 * groups. If so, move the start index accordingly
				 * (and so the other-file end-of-group index).
				 * Keep tracking the reference index in case we
				 * are shifting together with a corresponding
				 * group of changes in the other file.
				 */
				while (rchg[end])
					end++;

				io++;
				if (rchgo[io]) {
					end_matching_other = end;
					while (rchgo[io])
						io++;
				}
			}
		} while (groupsize != end - start);

		if (end == earliest_end)
			continue; /* no shifting is possible */

		/*
		 * The group can be shifted. Possibly use this freedom to produce
		 * a more intuitive diff.
		 *
		 * The group is currently shifted as far down as possible, so the
		 * heuristics below only have to handle upwards shifts.
		 */

		if ((flags & XDF_COMPACTION_HEURISTIC) && blank_lines) {
			/*
			 * Compaction heuristic: if possible, shift the group to
			 * make its bottom line a blank line.
			 */
			while (start > 0 &&
			       !is_blank_line(recs[end - 1], flags) &&
			       recs_match(recs, start - 1, end - 1, flags)) {
				rchg[--start] = 1;
				rchg[--end] = 0;

				io--;
				while (rchgo[io])
					io--;
			}
		} else if (end_matching_other != -1) {
			/*
			 * Move the possibly merged group of changes back to line
			 * up with the last group of changes from the other file
			 * that it can align with.
			 */
			while (end_matching_other < end) {
				rchg[--start] = 1;
				rchg[--end] = 0;

				io--;
				while (rchgo[io])
					io--;
			}
		} else if (flags & XDF_INDENT_HEURISTIC) {
			/*
			 * Indent heuristic: a group of pure add/delete lines
			 * implies two splits, one between the end of the "before"
			 * context and the start of the group, and another between
			 * the end of the group and the beginning of the "after"
			 * context. Some splits are aesthetically better and some
			 * are worse. We compute a badness "score" for each split,
			 * and add the scores for the two splits to define a
			 * "score" for each position that the group can be shifted
			 * to. Then we pick the shift with the lowest score.
			 */
			long shift, best_shift = -1;
			int best_score = 0;

			for (shift = earliest_end; shift <= end; shift++) {
				struct split_measurement m;
				int score;

				measure_split(xdf, shift, &m);
				score = score_split(&m);
				measure_split(xdf, shift - groupsize, &m);
				score += score_split(&m);
				if (best_shift == -1 || score <= best_score) {
					best_score = score;
					best_shift = shift;
				}
			}

			while (end > best_shift) {
				rchg[--start] = 1;
				rchg[--end] = 0;

				io--;
				while (rchgo[io])
					io--;
			}
		}
	}

	return 0;
}


int xdl_build_script(xdfenv_t *xe, xdchange_t **xscr) {
	xdchange_t *cscr = NULL, *xch;
	char *rchg1 = xe->xdf1.rchg, *rchg2 = xe->xdf2.rchg;
	long i1, i2, l1, l2;

	/*
	 * Trivial. Collects "groups" of changes and creates an edit script.
	 */
	for (i1 = xe->xdf1.nrec, i2 = xe->xdf2.nrec; i1 >= 0 || i2 >= 0; i1--, i2--)
		if (rchg1[i1 - 1] || rchg2[i2 - 1]) {
			for (l1 = i1; rchg1[i1 - 1]; i1--);
			for (l2 = i2; rchg2[i2 - 1]; i2--);

			if (!(xch = xdl_add_change(cscr, i1, i2, l1 - i1, l2 - i2))) {
				xdl_free_script(cscr);
				return -1;
			}
			cscr = xch;
		}

	*xscr = cscr;

	return 0;
}


void xdl_free_script(xdchange_t *xscr) {
	xdchange_t *xch;

	while ((xch = xscr) != NULL) {
		xscr = xscr->next;
		xdl_free(xch);
	}
}

static int xdl_call_hunk_func(xdfenv_t *xe, xdchange_t *xscr, xdemitcb_t *ecb,
			      xdemitconf_t const *xecfg)
{
	xdchange_t *xch, *xche;

	for (xch = xscr; xch; xch = xche->next) {
		xche = xdl_get_hunk(&xch, xecfg);
		if (!xch)
			break;
		if (xecfg->hunk_func(xch->i1, xche->i1 + xche->chg1 - xch->i1,
				     xch->i2, xche->i2 + xche->chg2 - xch->i2,
				     ecb->priv) < 0)
			return -1;
	}
	return 0;
}

static void xdl_mark_ignorable(xdchange_t *xscr, xdfenv_t *xe, long flags)
{
	xdchange_t *xch;

	for (xch = xscr; xch; xch = xch->next) {
		int ignore = 1;
		xrecord_t **rec;
		long i;

		rec = &xe->xdf1.recs[xch->i1];
		for (i = 0; i < xch->chg1 && ignore; i++)
			ignore = xdl_blankline(rec[i]->ptr, rec[i]->size, flags);

		rec = &xe->xdf2.recs[xch->i2];
		for (i = 0; i < xch->chg2 && ignore; i++)
			ignore = xdl_blankline(rec[i]->ptr, rec[i]->size, flags);

		xch->ignore = ignore;
	}
}

int xdl_diff(mmfile_t *mf1, mmfile_t *mf2, xpparam_t const *xpp,
	     xdemitconf_t const *xecfg, xdemitcb_t *ecb) {
	xdchange_t *xscr;
	xdfenv_t xe;
	emit_func_t ef = xecfg->hunk_func ? xdl_call_hunk_func : xdl_emit_diff;

	if (xdl_do_diff(mf1, mf2, xpp, &xe) < 0) {

		return -1;
	}
	if (xdl_change_compact(&xe.xdf1, &xe.xdf2, xpp->flags) < 0 ||
	    xdl_change_compact(&xe.xdf2, &xe.xdf1, xpp->flags) < 0 ||
	    xdl_build_script(&xe, &xscr) < 0) {

		xdl_free_env(&xe);
		return -1;
	}
	if (xscr) {
		if (xpp->flags & XDF_IGNORE_BLANK_LINES)
			xdl_mark_ignorable(xscr, &xe, xpp->flags);

		if (ef(&xe, xscr, ecb, xecfg) < 0) {

			xdl_free_script(xscr);
			xdl_free_env(&xe);
			return -1;
		}
		xdl_free_script(xscr);
	}
	xdl_free_env(&xe);

	return 0;
}
