/* rangetree.c -- binary trees of address ranges
 *
 * $Id$
 * Copyright (C) 2016-2018 Ravenbrook Limited.  See end of file for license.
 */

#include "rangetree.h"
#include "tree.h"
#include "range.h"
#include "mpm.h"


void RangeTreeInit(RangeTree rangeTree, Range range)
{
  AVER(rangeTree != NULL);
  TreeInit(RangeTreeTree(rangeTree));
  RangeCopy(RangeTreeRange(rangeTree), range);
  AVERT(RangeTree, rangeTree);
}


Bool RangeTreeCheck(RangeTree rangeTree)
{
  CHECKL(rangeTree != NULL);
  CHECKD_NOSIG(Tree, RangeTreeTree(rangeTree));
  CHECKD_NOSIG(Range, RangeTreeRange(rangeTree));
  return TRUE;
}


void RangeTreeFinish(RangeTree rangeTree)
{
  AVERT(RangeTree, rangeTree);
  TreeFinish(RangeTreeTree(rangeTree));
  RangeFinish(RangeTreeRange(rangeTree));
}


/* RangeTreeCompare -- Compare key to [base,limit)
 *
 * <design/splay#.type.splay.compare.method>
 */

Compare RangeTreeCompare(Tree tree, TreeKey key)
{
  Addr base1, base2, limit2;
  RangeTree block;

  AVERT_CRITICAL(Tree, tree);
  AVER_CRITICAL(tree != TreeEMPTY);
  AVER_CRITICAL(key != NULL);

  base1 = RangeTreeBaseOfKey(key);
  block = RangeTreeOfTree(tree);
  base2 = RangeTreeBase(block);
  limit2 = RangeTreeLimit(block);

  if (base1 < base2)
    return CompareLESS;
  else if (base1 >= limit2)
    return CompareGREATER;
  else
    return CompareEQUAL;
}

TreeKey RangeTreeKey(Tree tree)
{
  return RangeTreeKeyOfBaseVar(RangeTreeBase(RangeTreeOfTree(tree)));
}


/* C. COPYRIGHT AND LICENSE
 *
 * Copyright (C) 2016-2018 Ravenbrook Limited <http://www.ravenbrook.com/>.
 * All rights reserved.  This is an open source license.  Contact
 * Ravenbrook for commercial licensing options.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * 3. Redistributions in any form must be accompanied by information on how
 * to obtain complete source code for this software and any accompanying
 * software that uses this software.  The source code must either be
 * included in the distribution or be available for no more than the cost
 * of distribution plus a nominal fee, and must be freely redistributable
 * under reasonable conditions.  For an executable file, complete source
 * code means the source code for all modules it contains. It does not
 * include source code for modules or files that typically accompany the
 * major components of the operating system on which the executable file
 * runs.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE, OR NON-INFRINGEMENT, ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS AND CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
