/*
 * Copyright (c) 2014, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * cplxdeps.c
 *
 * normalize complex dependencies into CNF/DNF form
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

#include "pool.h"
#include "cplxdeps.h"

#ifdef ENABLE_COMPLEX_DEPS

#undef CPLXDEBUG

int
pool_is_complex_dep_rd(Pool *pool, Reldep *rd)
{
  for (;;)
    {
      if (rd->flags == REL_AND || rd->flags == REL_COND)	/* those two are the complex ones */
	return 1;
      if (rd->flags != REL_OR)
	return 0;
      if (ISRELDEP(rd->name) && pool_is_complex_dep_rd(pool, GETRELDEP(pool, rd->name)))
	return 1;
      if (!ISRELDEP(rd->evr))
	return 0;
      rd = GETRELDEP(pool, rd->evr);
    }
}

/* expand simple dependencies into package lists */
static int
expand_simpledeps(Pool *pool, Queue *bq, int start, int split)
{
  int end = bq->count;
  int i, x;
  int newsplit = 0;
  for (i = start; i < end; i++)
    {
      if (i == split)
	newsplit = bq->count - (end - start);
      x = bq->elements[i];
      if (x == pool->nsolvables)
	{
	  Id *dp = pool->whatprovidesdata + bq->elements[++i];
	  for (; *dp; dp++)
	    queue_push(bq, *dp);
	}
      else
	queue_push(bq, x);
    }
  if (i == split)
    newsplit = bq->count - (end - start);
  queue_deleten(bq, start, end - start);
  return newsplit;
}

#ifdef CPLXDEBUG
static void
print_depblocks(Pool *pool, Queue *bq, int start)
{
  int i;

  for (i = start; i < bq->count; i++)
    {
      if (bq->elements[i] == pool->nsolvables)
	{
	  Id *dp = pool->whatprovidesdata + bq->elements[++i];
	  printf(" (");
	  while (*dp)
	    printf(" %s", pool_solvid2str(pool, *dp++));
	  printf(" )");
	}
      else if (bq->elements[i] > 0)
	printf(" %s", pool_solvid2str(pool, bq->elements[i]));
      else if (bq->elements[i] < 0)
	printf(" -%s", pool_solvid2str(pool, -bq->elements[i]));
      else
	printf(" ||");
    }
  printf("\n");
}
#endif

/* invert all literals in the blocks. note that this also turns DNF into CNF and vice versa */
static int
invert_depblocks(Pool *pool, Queue *bq, int start, int r)
{
  int i, j, end;
  if (r == 0 || r == 1)
    return r ? 0 : 1;
  expand_simpledeps(pool, bq, start, 0);
  end = bq->count;
  for (i = j = start; i < end; i++)
    {
      if (bq->elements[i])
	{
          bq->elements[i] = -bq->elements[i];
	  continue;
	}
      /* end of block reached, reverse */
      if (i - 1 > j)
	{
	  int k;
	  for (k = i - 1; j < k; j++, k--)
	    {
	      Id t = bq->elements[j];
	      bq->elements[j] = bq->elements[k];
	      bq->elements[k] = t;
	    }
	}
      j = i + 1;
    }
  return -1;
}

/*
 * returns:
 *   0: no blocks
 *   1: matches all
 *  -1: at least one block
 */
static int
normalize_dep(Pool *pool, Id dep, Queue *bq, int flags)
{
  int bqcnt = bq->count;
  int bqcnt2;
  int todnf = flags & CPLXDEPS_TODNF ? 1 : 0;
  Id p, dp;

#ifdef CPLXDEBUG
  printf("normalize_dep %s todnf:%d\n", pool_dep2str(pool, dep), todnf);
#endif
  if (pool_is_complex_dep(pool, dep))
    {
      Reldep *rd = GETRELDEP(pool, dep);
      if (rd->flags == REL_AND || rd->flags == REL_OR || rd->flags == REL_COND)
	{
	  int rdflags = rd->flags;
	  Id name = rd->name;
	  Id evr = rd->evr;
	  int r, mode;
	  
          if (rdflags == REL_COND)
	    {
	      /* check for relly complex ELSE case */
	      if (ISRELDEP(evr))
		{
		  Reldep *rd2 = GETRELDEP(pool, evr);
		  if (rd2->flags == REL_ELSE)
		    {
		      int r2;
		      /* really complex case */
		      if ((flags & CPLXDEPS_ELSE_MASK) == CPLXDEPS_ELSE_AND_1)
			{
			  /* A OR ~B */
			  rdflags = REL_COND;
			  evr = rd2->name;
			}
		      else if ((flags & CPLXDEPS_ELSE_MASK) == CPLXDEPS_ELSE_AND_2)
			{
			  /* C OR B */
			  rdflags = REL_OR;
			  name = rd2->evr;
			  evr = rd2->name;
			}
		      else if ((flags & CPLXDEPS_ELSE_MASK) == CPLXDEPS_ELSE_OR_1)
			{
			  /* A AND B */
			  rdflags = REL_AND;
			  evr = rd2->name;
			}
		      else if ((flags & CPLXDEPS_ELSE_MASK) == CPLXDEPS_ELSE_OR_2)
			{
			  /* A AND C */
			  rdflags = REL_AND;
			  evr = rd2->evr;
			}
		      else if ((flags & CPLXDEPS_ELSE_MASK) == CPLXDEPS_ELSE_OR_3)
			{
			  /* C AND ~B */
			  rdflags = REL_ELSE;
			  name = rd2->evr;
			  evr = rd2->name;
			}
		      else if (!todnf)
			{
			  /* we want AND: A IF (B ELSE C) -> (A OR ~B) AND (C OR B) */
			  r = normalize_dep(pool, dep, bq, flags | CPLXDEPS_ELSE_AND_1);
			  if (r == 0 && (flags & CPLXDEPS_DONTFIX) == 0)
			    return 0;
			  r2 = normalize_dep(pool, dep, bq, flags | CPLXDEPS_ELSE_AND_2);
			  if (r2 == 0 && (flags & CPLXDEPS_DONTFIX) == 0)
			    {
			      queue_truncate(bq, bqcnt);
			      return 0;
			    }
			  if (r == -1 || r2 == -1)
			    return -1;
			  return r == 1 || r2 == 1 ? 1 : 0;
			}
		      else
			{
			  int r2, r3;
			  /* we want OR: A IF (B ELSE C) -> (A AND B) OR (A AND C) OR (~B AND C) */
			  r = normalize_dep(pool, dep, bq, flags | CPLXDEPS_ELSE_OR_1);
			  if (r == 1)
			    return 1;
			  r2 = normalize_dep(pool, dep, bq, flags | CPLXDEPS_ELSE_OR_2);
			  if (r2 == 1)
			    {
			      queue_truncate(bq, bqcnt);
			      return 1;
			    }
			  r3 = normalize_dep(pool, dep, bq, flags | CPLXDEPS_ELSE_OR_3);
			  if (r3 == 1)
			    {
			      queue_truncate(bq, bqcnt);
			      return 1;
			    }
			  if (r == -1 || r2 == -1 || r3 == -1)
			    return -1;
			  return 0;
			}
		    }
		}
	    }
	  mode = rdflags == REL_AND || rdflags == REL_ELSE ? 0 : 1;

	  /* get blocks of first argument */
	  r = normalize_dep(pool, name, bq, flags);
	  if (r == 0)
	    {
	      if (rdflags == REL_ELSE)
		return 0;
	      if (rdflags == REL_AND && (flags & CPLXDEPS_DONTFIX) == 0)
		return 0;
	      if (rdflags == REL_COND)
		{
		  r = normalize_dep(pool, evr, bq, (flags ^ CPLXDEPS_TODNF) & ~CPLXDEPS_DONTFIX);
		  return invert_depblocks(pool, bq, bqcnt, r);	/* invert block for COND */
		}
	      return normalize_dep(pool, evr, bq, flags);
	    }
	  if (r == 1)
	    {
	      if (rdflags == REL_ELSE)
		{
		  r = normalize_dep(pool, evr, bq, (flags ^ CPLXDEPS_TODNF) & ~CPLXDEPS_DONTFIX);
		  return invert_depblocks(pool, bq, bqcnt, r);	/* invert block for ELSE */
		}
	      if (rdflags == REL_OR || rdflags == REL_COND)
		return 1;
	      return normalize_dep(pool, evr, bq, flags);
	    }

	  /* get blocks of second argument */
	  bqcnt2 = bq->count;
	  /* COND is OR with NEG on evr block, so we invert the todnf flag in that case */
	  r = normalize_dep(pool, evr, bq, rdflags == REL_COND || rdflags == REL_ELSE ? ((flags ^ CPLXDEPS_TODNF) & ~CPLXDEPS_DONTFIX) : flags);
	  if (rdflags == REL_COND || rdflags == REL_ELSE)
	    r = invert_depblocks(pool, bq, bqcnt2, r);	/* invert 2nd block */
	  if (r == 0)
	    {
	      if (rdflags == REL_OR)
		return -1;
	      if (rdflags == REL_AND && (flags & CPLXDEPS_DONTFIX) != 0)
		return -1;
	      queue_truncate(bq, bqcnt);
	      return 0;
	    }
	  if (r == 1)
	    {
	      if (rdflags == REL_COND || rdflags == REL_OR)
		{
		  queue_truncate(bq, bqcnt);
		  return 1;
		}
	      return -1;
	    }
	  if (mode == todnf)
	    {
	      /* simple case: just join em. nothing more to do here. */
#ifdef CPLXDEBUG
	      printf("SIMPLE JOIN %d %d %d\n", bqcnt, bqcnt2, bq->count);
#endif
	      return -1;
	    }
	  else
	    {
	      /* complex case: mix em */
	      int i, j, bqcnt3;
#ifdef CPLXDEBUG
	      printf("COMPLEX JOIN %d %d %d\n", bqcnt, bqcnt2, bq->count);
#endif
	      bqcnt2 = expand_simpledeps(pool, bq, bqcnt, bqcnt2);
	      bqcnt3 = bq->count;
	      for (i = bqcnt; i < bqcnt2; i++)
		{
		  for (j = bqcnt2; j < bqcnt3; j++)
		    {
		      int a, b;
		      int bqcnt4 = bq->count;
		      int k = i;

		      /* mix i block with j block, both blocks are sorted */
		      while (bq->elements[k] && bq->elements[j])
			{
			  if (bq->elements[k] < bq->elements[j])
			    queue_push(bq, bq->elements[k++]);
			  else
			    {
			      if (bq->elements[k] == bq->elements[j])
				k++;
			      queue_push(bq, bq->elements[j++]);
			    }
			}
		      while (bq->elements[j])
			queue_push(bq, bq->elements[j++]);
		      while (bq->elements[k])
			queue_push(bq, bq->elements[k++]);

		      /* block is finished, check for A + -A */
		      for (a = bqcnt4, b = bq->count - 1; a < b; )
			{
			  if (-bq->elements[a] == bq->elements[b])
			    break;
			  if (-bq->elements[a] > bq->elements[b])
			    a++;
			  else
			    b--;
			}
		      if (a < b)
			queue_truncate(bq, bqcnt4);	/* ignore this block */
		      else
			queue_push(bq, 0);	/* finish block */
		    }
		  /* advance to next block */
		  while (bq->elements[i])
		    i++;
		}
	      i = -1;
	      if (bqcnt3 == bq->count)	/* ignored all blocks? */
		i = todnf ? 0 : 1;
	      queue_deleten(bq, bqcnt, bqcnt3 - bqcnt);
	      return i;
	    }
	}
    }

  /* fallback case: just use package list */
  dp = pool_whatprovides(pool, dep);
  if (dp <= 2 || !pool->whatprovidesdata[dp])
    return dp == 2 ? 1 : 0;
  if (pool->whatprovidesdata[dp] == SYSTEMSOLVABLE)
    return 1;
  bqcnt = bq->count;
  if ((flags & CPLXDEPS_NAME) != 0)
    {
      while ((p = pool->whatprovidesdata[dp++]) != 0)
	{
	  if (!pool_match_nevr(pool, pool->solvables + p, dep))
	    continue;
	  queue_push(bq, p);
	  if (todnf)
	    queue_push(bq, 0);
	}
    }
  else if (todnf)
    {
      while ((p = pool->whatprovidesdata[dp++]) != 0)
        queue_push2(bq, p, 0);
    }
  else
    queue_push2(bq, pool->nsolvables, dp);	/* not yet expanded marker + offset */
  if (bq->count == bqcnt)
    return 0;	/* no provider */
  if (!todnf)
    queue_push(bq, 0);	/* finish block */
  return -1;
}

int
pool_normalize_complex_dep(Pool *pool, Id dep, Queue *bq, int flags)
{
  int i, bqcnt = bq->count;

  i = normalize_dep(pool, dep, bq, flags);
  if ((flags & CPLXDEPS_EXPAND) != 0)
    {
      if (i != 0 && i != 1)
        expand_simpledeps(pool, bq, bqcnt, 0);
    }
  if ((flags & CPLXDEPS_INVERT) != 0)
    i = invert_depblocks(pool, bq, bqcnt, i);
#ifdef CPLXDEBUG
  if (i == 0)
    printf("NONE\n");
  else if (i == 1)
    printf("ALL\n");
  else
    print_depblocks(pool, bq, bqcnt);
#endif
  return i;
}

void
pool_add_pos_literals_complex_dep(Pool *pool, Id dep, Queue *q, Map *m, int neg)
{
  while (ISRELDEP(dep))
    {
      Reldep *rd = GETRELDEP(pool, dep);
      if (rd->flags != REL_AND && rd->flags != REL_OR && rd->flags != REL_COND)
	break;
      pool_add_pos_literals_complex_dep(pool, rd->name, q, m, neg);
      dep = rd->evr;
      if (rd->flags == REL_COND)
	{
	  neg = !neg;
	  if (ISRELDEP(dep))
	    {
	      Reldep *rd2 = GETRELDEP(pool, rd->evr);
	      if (rd2->flags == REL_ELSE)
		{
	          pool_add_pos_literals_complex_dep(pool, rd2->evr, q, m, !neg);
		  dep = rd2->name;
		}
	    }
	}
    }
  if (!neg)
    {
      Id p, pp;
      FOR_PROVIDES(p, pp, dep)
	if (!MAPTST(m, p))
	  queue_push(q, p);
    }
}

#endif	/* ENABLE_COMPLEX_DEPS */

