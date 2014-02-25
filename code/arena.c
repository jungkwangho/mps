/* arena.c: ARENA ALLOCATION FEATURES
 *
 * $Id$
 * Copyright (c) 2001 Ravenbrook Limited.  See end of file for license.
 *
 * .sources: <design/arena/> is the main design document.  */

#include "tract.h"
#include "poolmv.h"
#include "mpm.h"
#include "cbs.h"
#include "bt.h"


SRCID(arena, "$Id$");


#define ArenaControlPool(arena) MV2Pool(&(arena)->controlPoolStruct)
#define ArenaCBSBlockPool(arena)  (&(arena)->cbsBlockPoolStruct.poolStruct)
#define ArenaFreeCBS(arena)       (&(arena)->freeCBS)
#define ArenaZoneCBS(arena, z)    (&(arena)->zoneCBS[i])


/* Forward declarations */

static void ArenaTrivCompact(Arena arena, Trace trace);
static void arenaFreePage(Arena arena, Addr base, Pool pool);
static Res arenaCBSInit(Arena arena);
static void arenaCBSFinish(Arena arena);


/* ArenaTrivDescribe -- produce trivial description of an arena */

static Res ArenaTrivDescribe(Arena arena, mps_lib_FILE *stream)
{
  if (!TESTT(Arena, arena)) return ResFAIL;
  if (stream == NULL) return ResFAIL;

  /* .describe.triv.never-called-from-subclass-method:
   * This Triv method seems to assume that it will never get called
   * from a subclass-method invoking ARENA_SUPERCLASS()->describe.
   * It assumes that it only gets called if the describe method has
   * not been subclassed.  (That's the only reason for printing the
   * "No class-specific description available" message).
   * This is bogus, but that's the status quo.  RHSK 2007-04-27.
   */
  /* .describe.triv.dont-upcall: Therefore (for now) the last 
   * subclass describe method should avoid invoking 
   * ARENA_SUPERCLASS()->describe.  RHSK 2007-04-27.
   */
  return WriteF(stream,
    "  No class-specific description available.\n", NULL);
}


/* AbstractArenaClass  -- The abstract arena class definition
 *
 * .null: Most abstract class methods are set to NULL.  See
 * <design/arena/#class.abstract.null>.  */

typedef ArenaClassStruct AbstractArenaClassStruct;

DEFINE_CLASS(AbstractArenaClass, class)
{
  INHERIT_CLASS(&class->protocol, ProtocolClass);
  class->name = "ABSARENA";
  class->size = 0;
  class->offset = 0;
  class->varargs = ArgTrivVarargs;
  class->init = NULL;
  class->finish = NULL;
  class->reserved = NULL;
  class->purgeSpare = ArenaNoPurgeSpare;
  class->extend = ArenaNoExtend;
  class->grow = ArenaNoGrow;
  class->free = NULL;
  class->chunkInit = NULL;
  class->chunkFinish = NULL;
  class->compact = ArenaTrivCompact;
  class->describe = ArenaTrivDescribe;
  class->pagesMarkAllocated = NULL;
  class->sig = ArenaClassSig;
}


/* ArenaClassCheck -- check the consistency of an arena class */

Bool ArenaClassCheck(ArenaClass class)
{
  CHECKL(ProtocolClassCheck(&class->protocol));
  CHECKL(class->name != NULL); /* Should be <=6 char C identifier */
  CHECKL(class->size >= sizeof(ArenaStruct));
  /* Offset of generic Pool within class-specific instance cannot be */
  /* greater than the size of the class-specific portion of the */
  /* instance. */
  CHECKL(class->offset <= (size_t)(class->size - sizeof(ArenaStruct)));
  CHECKL(FUNCHECK(class->varargs));
  CHECKL(FUNCHECK(class->init));
  CHECKL(FUNCHECK(class->finish));
  CHECKL(FUNCHECK(class->reserved));
  CHECKL(FUNCHECK(class->purgeSpare));
  CHECKL(FUNCHECK(class->extend));
  CHECKL(FUNCHECK(class->free));
  CHECKL(FUNCHECK(class->chunkInit));
  CHECKL(FUNCHECK(class->chunkFinish));
  CHECKL(FUNCHECK(class->compact));
  CHECKL(FUNCHECK(class->describe));
  CHECKL(FUNCHECK(class->pagesMarkAllocated));
  CHECKS(ArenaClass, class);
  return TRUE;
}


/* ArenaCheck -- check the arena */

Bool ArenaCheck(Arena arena)
{
  Index i;

  CHECKS(Arena, arena);
  CHECKD(Globals, ArenaGlobals(arena));
  CHECKD(ArenaClass, arena->class);

  CHECKL(BoolCheck(arena->poolReady));
  if (arena->poolReady) { /* <design/arena/#pool.ready> */
    CHECKD(CBS, &arena->freeCBS);
    CHECKD(MV, &arena->controlPoolStruct);
    CHECKD(Reservoir, &arena->reservoirStruct);
  }

  for (i = 0; i < NELEMS(arena->freeRing); ++i)
    CHECKL(RingCheck(&arena->freeRing[i]));

  /* Can't check that limit>=size because we may call ArenaCheck */
  /* while the size is being adjusted. */

  CHECKL(arena->committed <= arena->commitLimit);
  CHECKL(arena->spareCommitted <= arena->committed);

  CHECKL(ShiftCheck(arena->zoneShift));
  CHECKL(AlignCheck(arena->alignment));
  /* Tract allocation must be platform-aligned. */
  CHECKL(arena->alignment >= MPS_PF_ALIGN);
  /* Stripes can't be smaller than pages. */
  CHECKL(((Size)1 << arena->zoneShift) >= arena->alignment);

  if (arena->lastTract == NULL) {
    CHECKL(arena->lastTractBase == (Addr)0);
  } else {
    CHECKL(TractBase(arena->lastTract) == arena->lastTractBase);
  }

  if (arena->primary != NULL) {
    CHECKD(Chunk, arena->primary);
  }
  CHECKL(RingCheck(&arena->chunkRing));
  /* nothing to check for chunkSerial */
  CHECKD(ChunkCacheEntry, &arena->chunkCache);
  
  CHECKL(LocusCheck(arena));

  /* FIXME: Check CBSs */
  AVER(NELEMS(arena->zoneCBS) == sizeof(ZoneSet) * CHAR_BIT);
  
  return TRUE;
}


/* ArenaInit -- initialize the generic part of the arena
 *
 * .init.caller: Unlike PoolInit, this is called by the class init
 * methods, not the generic Create.  This is because the class is
 * responsible for allocating the descriptor.  */

Res ArenaInit(Arena arena, ArenaClass class, Align alignment)
{
  Res res;
  Index i;

  /* We do not check the arena argument, because it's _supposed_ to */
  /* point to an uninitialized block of memory. */
  AVERT(ArenaClass, class);

  arena->class = class;

  arena->committed = (Size)0;
  /* commitLimit may be overridden by init (but probably not */
  /* as there's not much point) */
  arena->commitLimit = (Size)-1;
  arena->spareCommitted = (Size)0;
  arena->spareCommitLimit = ARENA_INIT_SPARE_COMMIT_LIMIT;
  arena->alignment = alignment;
  /* zoneShift is usually overridden by init */
  arena->zoneShift = ARENA_ZONESHIFT;
  arena->poolReady = FALSE;     /* <design/arena/#pool.ready> */
  arena->lastTract = NULL;
  arena->lastTractBase = NULL;
  arena->hasFreeCBS = FALSE;
  arena->freeZones = ZoneSetUNIV;

  arena->primary = NULL;
  RingInit(&arena->chunkRing);
  arena->chunkSerial = (Serial)0;
  ChunkCacheEntryInit(&arena->chunkCache);
  
  for (i = 0; i < NELEMS(arena->freeRing); ++i)
    RingInit(&arena->freeRing[i]);

  LocusInit(arena);
  
  res = GlobalsInit(ArenaGlobals(arena));
  if (res != ResOK)
    goto failGlobalsInit;

  arena->sig = ArenaSig;
  
  res = arenaCBSInit(arena);
  if (res != ResOK)
    goto failCBSInit;
  
  /* initialize the reservoir, <design/reservoir/> */
  res = ReservoirInit(&arena->reservoirStruct, arena);
  if (res != ResOK)
    goto failReservoirInit;

  AVERT(Arena, arena);
  return ResOK;

failReservoirInit:
  arenaCBSFinish(arena);
failCBSInit:
  GlobalsFinish(ArenaGlobals(arena));
failGlobalsInit:
  return res;
}


/* VM keys are defined here even though the code they apply to might
 * not be linked.  For example, MPS_KEY_VMW3_TOP_DOWN only applies to
 * vmw3.c.  The reason is that we want these keywords to be optional
 * even on the wrong platform, so that clients can write simple portable
 * code.  They should be free to pass MPS_KEY_VMW3_TOP_DOWN on other
 * platforms, knowing that it has no effect.  To do that, the key must
 * exist on all platforms. */

ARG_DEFINE_KEY(vmw3_top_down, Bool);


/* ArenaCreate -- create the arena and call initializers */

ARG_DEFINE_KEY(arena_size, Size);

Res ArenaCreate(Arena *arenaReturn, ArenaClass class, ArgList args)
{
  Arena arena;
  Res res;

  AVER(arenaReturn != NULL);
  AVERT(ArenaClass, class);
  AVER(ArgListCheck(args));

  /* We must initialise the event subsystem very early, because event logging
     will start as soon as anything interesting happens and expect to write
     to the EventLast pointers. */
  EventInit();

  /* Do initialization.  This will call ArenaInit (see .init.caller). */
  res = (*class->init)(&arena, class, args);
  if (res != ResOK)
    goto failInit;

  /* arena->alignment must have been set up by *class->init() */
  if (arena->alignment > ((Size)1 << arena->zoneShift)) {
    res = ResMEMORY; /* size was too small */
    goto failStripeSize;
  }

  /* With the primary chunk initialised we can add page memory to the freeCBS
     that describes the free address space in the primary chunk. */
  arena->hasFreeCBS = TRUE;
  res = ArenaFreeCBSInsert(arena,
                           PageIndexBase(arena->primary,
                                         arena->primary->allocBase),
                           arena->primary->limit);
  if (res != ResOK)
    goto failPrimaryCBS;
  
  res = ControlInit(arena);
  if (res != ResOK)
    goto failControlInit;

  res = GlobalsCompleteCreate(ArenaGlobals(arena));
  if (res != ResOK)
    goto failGlobalsCompleteCreate;

  AVERT(Arena, arena);
  *arenaReturn = arena;
  return ResOK;

failGlobalsCompleteCreate:
  ControlFinish(arena);
failControlInit:
failPrimaryCBS:
failStripeSize:
  (*class->finish)(arena);
failInit:
  return res;
}


/* ArenaFinish -- finish the generic part of the arena
 *
 * .finish.caller: Unlike PoolFinish, this is called by the class finish
 * methods, not the generic Destroy.  This is because the class is
 * responsible for deallocating the descriptor.  */

void ArenaFinish(Arena arena)
{
  Index i;
  ReservoirFinish(ArenaReservoir(arena));
  for (i = 0; i < NELEMS(arena->freeRing); ++i)
    RingFinish(&arena->freeRing[i]);
  arena->sig = SigInvalid;
  GlobalsFinish(ArenaGlobals(arena));
  LocusFinish(arena);
  RingFinish(&arena->chunkRing);
}


/* ArenaDestroy -- destroy the arena */

void ArenaDestroy(Arena arena)
{

  AVERT(Arena, arena);

  GlobalsPrepareToDestroy(ArenaGlobals(arena));

  /* Empty the reservoir - see <code/reserv.c#reservoir.finish> */
  ReservoirSetLimit(ArenaReservoir(arena), 0);

  arena->poolReady = FALSE;
  ControlFinish(arena);

  arenaCBSFinish(arena);

  /* Call class-specific finishing.  This will call ArenaFinish. */
  (*arena->class->finish)(arena);

  EventFinish();
}


/* ControlInit -- initialize the control pool */

Res ControlInit(Arena arena)
{
  Res res;

  AVERT(Arena, arena);
  MPS_ARGS_BEGIN(args) {
    MPS_ARGS_ADD(args, MPS_KEY_EXTEND_BY, CONTROL_EXTEND_BY);
    MPS_ARGS_DONE(args);
    res = PoolInit(&arena->controlPoolStruct.poolStruct, arena,
                   PoolClassMV(), args);
  } MPS_ARGS_END(args);
  if (res != ResOK)
    return res;
  arena->poolReady = TRUE;      /* <design/arena/#pool.ready> */
  return ResOK;
}


/* ControlFinish -- finish the control pool */

void ControlFinish(Arena arena)
{
  AVERT(Arena, arena);
  arena->poolReady = FALSE;
  PoolFinish(&arena->controlPoolStruct.poolStruct);
}


/* ArenaDescribe -- describe the arena */

Res ArenaDescribe(Arena arena, mps_lib_FILE *stream)
{
  Res res;
  Size reserved;

  if (!TESTT(Arena, arena)) return ResFAIL;
  if (stream == NULL) return ResFAIL;

  res = WriteF(stream, "Arena $P {\n", (WriteFP)arena,
               "  class $P (\"$S\")\n",
               (WriteFP)arena->class, arena->class->name,
               NULL);
  if (res != ResOK) return res;

  if (arena->poolReady) {
    res = WriteF(stream,
                 "  controlPool $P\n", (WriteFP)&arena->controlPoolStruct,
                 NULL);
    if (res != ResOK) return res;
  }

  /* Note: this Describe clause calls a function */
  reserved = ArenaReserved(arena);
  res = WriteF(stream,
               "  reserved         $W  <-- "
               "total size of address-space reserved\n",
               (WriteFW)reserved,
               NULL);
  if (res != ResOK) return res;

  res = WriteF(stream,
               "  committed        $W  <-- "
               "total bytes currently stored (in RAM or swap)\n",
               (WriteFW)arena->committed,
               "  commitLimit      $W\n", (WriteFW)arena->commitLimit,
               "  spareCommitted   $W\n", (WriteFW)arena->spareCommitted,
               "  spareCommitLimit $W\n", (WriteFW)arena->spareCommitLimit,
               "  zoneShift $U\n", (WriteFU)arena->zoneShift,
               "  alignment $W\n", (WriteFW)arena->alignment,
               NULL);
  if (res != ResOK) return res;

  res = WriteF(stream,
               "  droppedMessages $U$S\n", (WriteFU)arena->droppedMessages,
               (arena->droppedMessages == 0 ? "" : "  -- MESSAGES DROPPED!"),
               NULL);
  if (res != ResOK) return res;

  res = (*arena->class->describe)(arena, stream);
  if (res != ResOK) return res;

  /* Do not call GlobalsDescribe: it makes too much output, thanks.
   * RHSK 2007-04-27
   */
#if 0
  res = GlobalsDescribe(ArenaGlobals(arena), stream);
  if (res != ResOK) return res;
#endif

  res = WriteF(stream,
               "} Arena $P ($U)\n", (WriteFP)arena,
               (WriteFU)arena->serial,
               NULL);
  return res;
}


/* ArenaDescribeTracts -- describe all the tracts in the arena */

Res ArenaDescribeTracts(Arena arena, mps_lib_FILE *stream)
{
  Res res;
  Tract tract;
  Bool b;
  Addr oldLimit, base, limit;
  Size size;

  if (!TESTT(Arena, arena)) return ResFAIL;
  if (stream == NULL) return ResFAIL;

  b = TractFirst(&tract, arena);
  oldLimit = TractBase(tract);
  while (b) {
    base = TractBase(tract);
    limit = TractLimit(tract);
    size = ArenaAlign(arena);

    if (TractBase(tract) > oldLimit) {
      res = WriteF(stream,
                   "[$P, $P) $W $U   ---\n",
                   (WriteFP)oldLimit, (WriteFP)base,
                   (WriteFW)AddrOffset(oldLimit, base),
                   (WriteFU)AddrOffset(oldLimit, base),
                   NULL);
      if (res != ResOK) return res;
    }

    res = WriteF(stream,
                 "[$P, $P) $W $U   $P ($S)\n",
                 (WriteFP)base, (WriteFP)limit,
                 (WriteFW)size, (WriteFW)size,
                 (WriteFP)TractPool(tract),
                 (WriteFS)(TractPool(tract)->class->name),
                 NULL);
    if (res != ResOK) return res;
    b = TractNext(&tract, arena, TractBase(tract));
    oldLimit = limit;
  }
  return ResOK;
}


/* ControlAlloc -- allocate a small block directly from the control pool
 *
 * .arena.control-pool: Actually the block will be allocated from the
 * control pool, which is an MV pool embedded in the arena itself.
 *
 * .controlalloc.addr: In implementations where Addr is not compatible
 * with void* (<design/type/#addr.use>), ControlAlloc must take care of
 * allocating so that the block can be addressed with a void*.  */

Res ControlAlloc(void **baseReturn, Arena arena, size_t size,
                 Bool withReservoirPermit)
{
  Addr base;
  Res res;

  AVERT(Arena, arena);
  AVER(baseReturn != NULL);
  AVER(size > 0);
  AVER(BoolCheck(withReservoirPermit));
  AVER(arena->poolReady);

  res = PoolAlloc(&base, ArenaControlPool(arena), (Size)size,
                  withReservoirPermit);
  if (res != ResOK)
    return res;

  *baseReturn = (void *)base; /* see .controlalloc.addr */
  return ResOK;
}


/* ControlFree -- free a block allocated using ControlAlloc */

void ControlFree(Arena arena, void* base, size_t size)
{
  AVERT(Arena, arena);
  AVER(base != NULL);
  AVER(size > 0);
  AVER(arena->poolReady);

  PoolFree(ArenaControlPool(arena), (Addr)base, (Size)size);
}


/* ControlDescribe -- describe the arena's control pool */

Res ControlDescribe(Arena arena, mps_lib_FILE *stream)
{
  Res res;

  if (!TESTT(Arena, arena)) return ResFAIL;
  if (stream == NULL) return ResFAIL;

  res = PoolDescribe(ArenaControlPool(arena), stream);

  return res;
}


/* arenaAllocPage -- allocate one page from the arena
 *
 * This is a primitive allocator used to allocate pages for the arena CBS.
 * It is called rarely and can use a simple search.  It may not use the
 * CBS or any pool, because it is used as part of the bootstrap.
 *
 * FIXME: Might this allocate a page that is in a free CBS?
 */

static Res arenaAllocPageInChunk(Addr *baseReturn, Chunk chunk, Pool pool)
{
  Res res;
  Index basePageIndex, limitPageIndex;
  Arena arena;

  AVER(baseReturn != NULL);
  AVERT(Chunk, chunk);
  AVERT(Pool, pool);
  arena = ChunkArena(chunk);
  
  if (!BTFindShortResRange(&basePageIndex, &limitPageIndex,
                           chunk->allocTable,
                           chunk->allocBase, chunk->pages, 1))
    return ResRESOURCE;
  
  res = (*arena->class->pagesMarkAllocated)(arena, chunk,
                                            basePageIndex, 1,
                                            pool);
  if (res != ResOK)
    return res;
  
  *baseReturn = PageIndexBase(chunk, basePageIndex);
  return ResOK;
}

static Res arenaAllocPage(Addr *baseReturn, Arena arena, Pool pool)
{
  Res res;
  
  /* Favour the primary pool, because pages allocated this way aren't
     currently freed, and we don't want to prevent chunks being destroyed. */
  res = arenaAllocPageInChunk(baseReturn, arena->primary, pool);
  if (res != ResOK) {
    Ring node, next;
    RING_FOR(node, &arena->chunkRing, next) {
      Chunk chunk = RING_ELT(Chunk, chunkRing, node);
      if (chunk != arena->primary) {
        res = arenaAllocPageInChunk(baseReturn, chunk, pool);
        if (res == ResOK)
          break;
      }
    }
  }
  return res;
}

static void arenaFreePage(Arena arena, Addr base, Pool pool)
{
  AVERT(Arena, arena);
  AVERT(Pool, pool);
  (*arena->class->free)(base, ArenaAlign(arena), pool);
}


/* arenaFreeCBSInsert -- add block to free CBS, extending pool if necessary
 *
 * The arena's freeCBS can't get memory in the usual way because it is used
 * in the basic allocator, so we allocate pages specially.
 */

Res ArenaFreeCBSInsert(Arena arena, Addr base, Addr limit)
{
  Pool pool = &arena->cbsBlockPoolStruct.poolStruct;
  RangeStruct range;
  Res res;

  RangeInit(&range, base, limit);
  res = CBSInsert(&range, &arena->freeCBS, &range);
  if (res == ResLIMIT) { /* freeCBS MFS pool ran out of blocks */
    Addr pageBase;

    res = arenaAllocPage(&pageBase, arena, pool);
    if (res != ResOK)
      return res;

    MFSExtend(pool, pageBase, arena->alignment);

    /* Add the chunk's whole free area to the arena's CBS. */
    res = CBSInsert(&range, &arena->freeCBS, &range);
    AVER(res == ResOK); /* we just gave memory to the CBS */

    /* Exclude the page we specially allocated for the MFS from the CBS
       so that it doesn't get reallocated. */
    RangeInit(&range, pageBase, AddrAdd(pageBase, arena->alignment));
    res = CBSDelete(&range, &arena->freeCBS, &range);
    AVER(res == ResOK); /* we just gave memory to the CBS */
  }
  
  return ResOK;
}


/* ArenaFreeCBSDelete -- remove a block from free CBS, extending pool if necessary
 *
 * See ArenaFreeCBSInsert.
 */

void ArenaFreeCBSDelete(Arena arena, Addr base, Addr limit)
{
  RangeStruct range;
  Res res;
  Count nodes;

  RangeInit(&range, base, limit);
  nodes = arena->freeCBS.splayTreeSize;
  res = CBSDelete(&range, &arena->freeCBS, &range);
  
  /* This should never fail because it is only used to delete whole chunks
     that are represented by single nodes in the CBS tree. */
  /* FIXME: Need a better way of checking this. */
  STATISTIC_STAT(AVER(arena->freeCBS.splayTreeSize == nodes - 1));
  AVER(res == ResOK);
}


/* arenaCBSInit -- initialise the arena's free CBSs */

static Res arenaCBSInit(Arena arena)
{
  Res res;
  Index i;

  AVERT(Arena, arena);
  AVER(!arena->hasFreeCBS);

  /* Initialise a pool to hold the arena's CBS blocks.  This pool can't be
     allowed to extend itself using ArenaAlloc because it is used during
     ArenaAlloc, so MFSExtendSelf is set to FALSE.  Failures to extend are
     handled where the CBS is used. */

  MPS_ARGS_BEGIN(piArgs) {
    MPS_ARGS_ADD(piArgs, MPS_KEY_MFS_UNIT_SIZE, sizeof(CBSBlockStruct));
    MPS_ARGS_ADD(piArgs, MPS_KEY_EXTEND_BY, arena->alignment);
    MPS_ARGS_ADD(piArgs, MFSExtendSelf, FALSE); /* FIXME: Explain why */
    MPS_ARGS_DONE(piArgs);
    res = PoolInit(ArenaCBSBlockPool(arena), arena, PoolClassMFS(), piArgs);
  } MPS_ARGS_END(piArgs);
  AVER(res == ResOK); /* no allocation, no failure expected */
  if (res != ResOK)
    goto failMFSInit;

  /* Initialise the freeCBS. */
  MPS_ARGS_BEGIN(cbsiArgs) {
    MPS_ARGS_ADD(cbsiArgs, CBSBlockPool, ArenaCBSBlockPool(arena));
    MPS_ARGS_DONE(cbsiArgs);
    res = CBSInit(arena, ArenaFreeCBS(arena), arena,
                  arena->alignment, TRUE, cbsiArgs);
  } MPS_ARGS_END(cbsiArgs);
  AVER(res == ResOK); /* no allocation, no failure expected */
  if (res != ResOK)
    goto failCBSInit;
  /* Note that although freeCBS is initialised, it doesn't have any memory
     for its blocks, so hasFreeCBS remains FALSE until later. */

  /* Initialise the zoneCBSs. */
  for (i = 0; i < NELEMS(arena->zoneCBS); ++i) {
    MPS_ARGS_BEGIN(cbsiArgs) {
      MPS_ARGS_ADD(cbsiArgs, CBSBlockPool, ArenaCBSBlockPool(arena));
      MPS_ARGS_DONE(cbsiArgs);
      res = CBSInit(arena, ArenaZoneCBS(arena, i), arena,
                    arena->alignment, TRUE, cbsiArgs);
    } MPS_ARGS_END(cbsiArgs);
    AVER(res == ResOK); /* no allocation, no failure expected */
    if (res != ResOK)
      goto failZoneCBSInit;
  }
  
  return ResOK;

failZoneCBSInit:
  while (i > 0) {
    --i;
    CBSFinish(&arena->zoneCBS[i]);
  }
  CBSFinish(&arena->freeCBS);
failCBSInit:
  PoolFinish(ArenaCBSBlockPool(arena));
failMFSInit:
  return res;
}


/* arenaCBSFinish -- finish the arena's free CBSs */

static void arenaMFSPageFreeVisitor(Pool pool, Addr base, Size size,
                                    void *closureP, Size closureS)
{
  AVERT(Pool, pool);
  UNUSED(closureP);
  UNUSED(closureS);
  UNUSED(size);
  AVER(size == ArenaAlign(PoolArena(pool)));
  arenaFreePage(PoolArena(pool), base, pool);
}

static void arenaCBSFinish(Arena arena)
{
  Index i;
  
  AVERT(Arena, arena);

  /* We must tear down the freeCBS before the chunks, because pages
     containing CBS blocks might be allocated in those chunks. */
  AVER(arena->hasFreeCBS);
  arena->hasFreeCBS = FALSE;
  for (i = 0; i < NELEMS(arena->zoneCBS); ++i)
    CBSFinish(&arena->zoneCBS[i]);
  CBSFinish(&arena->freeCBS);
  
  /* The CBS block pool can't free its own memory via ArenaFree because
     that would use the freeCBS. */
  MFSFinishTracts(ArenaCBSBlockPool(arena),
                  arenaMFSPageFreeVisitor, NULL, 0);
  PoolFinish(ArenaCBSBlockPool(arena));
}


/* arenaAllocFromCBS -- allocate memory using the free CBS
 *
 * The free CBS contains all the free address space we have in chunks,
 * so this is the primary method of allocation.
 * FIXME: Needs to take a "high" option to use CBSFindLastInZones.
 */

static Bool arenaAllocFindSpare(Chunk *chunkReturn,
                                Index *baseIndexReturn,
                                Count *pagesReturn,
                                Arena arena, ZoneSet zones, Size size)
{
  Index i;
  
  if (size == ArenaAlign(arena)) {
    for (i = 0; i < NELEMS(arena->freeRing); ++i) {
      Ring ring = &arena->freeRing[i];
      if (ZoneSetIsMember(zones, i) && !RingIsSingle(ring)) {
        Page page = PageOfFreeRing(RingNext(ring));
        Chunk chunk;
        Bool b = ChunkOfAddr(&chunk, arena, (Addr)page);
        AVER(b);
        AVER(ChunkPageSize(chunk) == size);
        *chunkReturn = chunk;
        *baseIndexReturn = (Index)(page - chunk->pageTable);
        *pagesReturn = 1;
        return TRUE;
      }
    }
  }
  return FALSE;
}

static Bool arenaAllocFindInZoneCBS(Range rangeReturn,
                                    Arena arena, ZoneSet zones, Size size)
{
  Index i;
  RangeStruct oldRange;
  for (i = 0; i < NELEMS(arena->zoneCBS); ++i)
    if (ZoneSetIsMember(zones, i) &&
        CBSFindFirst(rangeReturn, &oldRange, &arena->zoneCBS[i], size,
                     FindDeleteLOW)) /* FIXME: use HIGH according to segpref */
      return TRUE;
  return FALSE;
}

static Bool arenaAllocFindInFreeCBS(Range rangeReturn,
                                    Arena arena, ZoneSet zones, Size size)
{
  Res res;
  RangeStruct oldRange;
  
  if (CBSFindFirstInZones(rangeReturn, &oldRange, &arena->freeCBS, size,
                          arena, zones)) {
    Addr allocLimit = RangeLimit(rangeReturn);
    Addr stripeLimit = AddrAlignUp(allocLimit, (Size)1 << ArenaZoneShift(arena));
    Addr oldLimit = RangeLimit(&oldRange);
    Addr limit = oldLimit < stripeLimit ? oldLimit : stripeLimit;
    RangeStruct restRange;
    CBS zoneCBS;
    RangeInit(&restRange, allocLimit, limit);
    AVER(RangesNest(&oldRange, &restRange));
    if (allocLimit < limit) { /* FIXME: RangeIsEmpty */
      res = CBSDelete(&oldRange, &arena->freeCBS, &restRange);
      AVER(res == ResOK); /* we should just be bumping up a base */
      zoneCBS = &arena->zoneCBS[AddrZone(arena, RangeBase(&restRange))];
      res = CBSInsert(&oldRange, zoneCBS, &restRange);
      AVER(res == ResOK); /* FIXME: might not be! */
    }
    return TRUE;
  }
  return FALSE;
}

static Res arenaAllocFromCBS(Tract *tractReturn, ZoneSet zones,
                             Size size, Pool pool)
{
  Arena arena;
  RangeStruct range, oldRange;
  Chunk chunk;
  Bool b;
  Index baseIndex;
  Count pages;
  Res res;
  
  AVER(tractReturn != NULL);
  /* ZoneSet is arbitrary */
  AVER(size > (Size)0);
  AVERT(Pool, pool);
  arena = PoolArena(pool);
  AVER(SizeIsAligned(size, arena->alignment));
  
  /* TODO: What about a range that crosses chunks?! Every chunk has
     some unallocated space at the beginning with page tables in it.
     This assumption needs documenting and asserting! */

  if (arenaAllocFindSpare(&chunk, &baseIndex, &pages, arena, zones, size)) {
    Addr base = PageIndexBase(chunk, baseIndex);
    RangeInit(&range, base, AddrAdd(base, ChunkPageSize(chunk) * pages));
  } else {
    if (!arenaAllocFindInZoneCBS(&range, arena, zones, size))
      if (!arenaAllocFindInFreeCBS(&range, arena, zones, size))
        return ResRESOURCE;
    b = CHUNK_OF_ADDR(&chunk, arena, range.base);
    AVER(b);
    AVER(RangeIsAligned(&range, ChunkPageSize(chunk)));
    baseIndex = INDEX_OF_ADDR(chunk, range.base);
    pages = ChunkSizeToPages(chunk, RangeSize(&range));
  }

  res = (*arena->class->pagesMarkAllocated)(arena, chunk, baseIndex, pages, pool);
  if (res != ResOK)
    goto failMark;

  arena->freeZones = ZoneSetDiff(arena->freeZones,
                                 ZoneSetOfRange(arena, range.base, range.limit));

  *tractReturn = PageTract(&chunk->pageTable[baseIndex]); /* FIXME: method for this? */
  return ResOK;

failMark:
   NOTREACHED; /* FIXME */
   {
     Res insertRes = CBSInsert(&oldRange, &arena->freeCBS, &range);
     AVER(insertRes == ResOK); /* We only just deleted it. */
     /* If the insert does fail, we lose some address space permanently. */
   }
   return res;
}


/* arenaAllocPolicy -- arena allocation policy implementation
 *
 * This is the code responsible for making decisions about where to allocate
 * memory.  Avoid distributing code for doing this elsewhere, so that policy
 * can be maintained and adjusted.
 */

static Res arenaAllocPolicy(Tract *tractReturn, Arena arena, SegPref pref,
                            Size size, Pool pool)
{
  Res res;
  Tract tract;
  ZoneSet zones, moreZones, evenMoreZones;

  AVER(tractReturn != NULL);
  AVERT(SegPref, pref);
  AVER(size > (Size)0);
  AVERT(Pool, pool);
  
  /* FIXME: Allow arena to take an option to ignore zones. */
  /* FIXME: Respect pref->high and other fields */

  /* Don't attempt to allocate if doing so would definitely exceed the
     commit limit. */
  if (arena->spareCommitted < size) {
    Size necessaryCommitIncrease = size - arena->spareCommitted;
    if (arena->committed + necessaryCommitIncrease > arena->commitLimit
        || arena->committed + necessaryCommitIncrease < arena->committed) {
      return ResCOMMIT_LIMIT;
    }
  }
  
  /* Plan A: allocate from the free CBS in the requested zones */
  /* FIXME: Takes no account of other zones fields */
  zones = pref->zones;
  if (zones != ZoneSetEMPTY) {
    res = arenaAllocFromCBS(&tract, zones, size, pool);
    if (res == ResOK)
      goto found;
  }

  /* Plan B: add free zones that aren't blacklisted */
  /* TODO: Pools without ambiguous roots might not care about the blacklist. */
  /* TODO: zones are precious and (currently) never deallocated, so we
     should consider extending the arena first if address space is plentiful */
  moreZones = ZoneSetUnion(zones, ZoneSetDiff(arena->freeZones, pref->avoid));
  if (moreZones != zones) {
    res = arenaAllocFromCBS(&tract, moreZones, size, pool);
    if (res == ResOK)
      goto found;
  }
  
  /* Plan C: Extend the arena, then try A and B again. */
  if (moreZones != ZoneSetEMPTY) {
    res = arena->class->grow(arena, pref, size);
    if (res != ResOK)
      return res;
    zones = pref->zones;
    if (zones != ZoneSetEMPTY) {
      res = arenaAllocFromCBS(&tract, zones, size, pool);
      if (res == ResOK)
        goto found;
    }
    if (moreZones != zones) {
      zones = ZoneSetUnion(zones, ZoneSetDiff(arena->freeZones, pref->avoid));
      res = arenaAllocFromCBS(&tract, moreZones, size, pool);
      if (res == ResOK)
        goto found;
    }
  }

  /* Plan D: add every zone that isn't blacklisted.  This might mix GC'd
     objects with those from other generations, causing the zone check
     to give false positives and slowing down the collector. */
  /* TODO: log an event for this */
  evenMoreZones = ZoneSetUnion(moreZones, ZoneSetDiff(ZoneSetUNIV, pref->avoid));
  if (evenMoreZones != moreZones) {
    res = arenaAllocFromCBS(&tract, evenMoreZones, size, pool);
    if (res == ResOK)
      goto found;
  }

  /* Last resort: try anywhere.  This might put GC'd objects in zones where
     common ambiguous bit patterns pin them down, causing the zone check
     to give even more false positives permanently, and possibly retaining
     garbage indefinitely. */
  res = arenaAllocFromCBS(&tract, ZoneSetUNIV, size, pool);
  if (res == ResOK)
    goto found;

  /* Uh oh. */
  return res;

found:
  *tractReturn = tract;
  return ResOK;
}


/* ArenaAlloc -- allocate some tracts from the arena */

Res ArenaAlloc(Addr *baseReturn, SegPref pref, Size size, Pool pool,
               Bool withReservoirPermit)
{
  Res res;
  Arena arena;
  Addr base;
  Tract tract;
  Reservoir reservoir;

  AVER(baseReturn != NULL);
  AVERT(SegPref, pref);
  AVER(size > (Size)0);
  AVERT(Pool, pool);
  AVER(BoolCheck(withReservoirPermit));

  arena = PoolArena(pool);
  AVERT(Arena, arena);
  AVER(SizeIsAligned(size, arena->alignment));
  reservoir = ArenaReservoir(arena);
  AVERT(Reservoir, reservoir);

  if (pool != ReservoirPool(reservoir)) {
    res = ReservoirEnsureFull(reservoir);
    if (res != ResOK) {
      AVER(ResIsAllocFailure(res));
      if (!withReservoirPermit)
        return res;
    }
  }

  res = arenaAllocPolicy(&tract, arena, pref, size, pool);
  if (res != ResOK) {
    if (withReservoirPermit) {
      Res resRes = ReservoirWithdraw(&base, &tract, reservoir, size, pool);
      if (resRes != ResOK)
        goto allocFail;
    } else
      goto allocFail;
  }
  
  base = TractBase(tract);

  /* cache the tract - <design/arena/#tract.cache> */
  arena->lastTract = tract;
  arena->lastTractBase = base;

  EVENT5(ArenaAlloc, arena, tract, base, size, pool);

  *baseReturn = base;
  return ResOK;

allocFail:
   EVENT3(ArenaAllocFail, arena, size, pool); /* TODO: Should have res? */
   return res;
}


/* ArenaFree -- free some tracts to the arena */

void ArenaFree(Addr base, Size size, Pool pool)
{
  Arena arena;
  Addr limit;
  Reservoir reservoir;
  Res res;
  Addr wholeBase;
  Size wholeSize;

  AVERT(Pool, pool);
  AVER(base != NULL);
  AVER(size > (Size)0);
  arena = PoolArena(pool);
  AVERT(Arena, arena);
  reservoir = ArenaReservoir(arena);
  AVERT(Reservoir, reservoir);
  AVER(AddrIsAligned(base, arena->alignment));
  AVER(SizeIsAligned(size, arena->alignment));

  /* uncache the tract if in range - <design/arena/#tract.uncache> */
  limit = AddrAdd(base, size);
  if ((arena->lastTractBase >= base) && (arena->lastTractBase < limit)) {
    arena->lastTract = NULL;
    arena->lastTractBase = (Addr)0;
  }
  
  wholeBase = base;
  wholeSize = size;

  if (pool != ReservoirPool(reservoir)) {
    res = ReservoirEnsureFull(reservoir);
    if (res != ResOK) {
      AVER(ResIsAllocFailure(res));
      /* FIXME: This appears to deposit the whole area into the reservoir
         no matter how big it is, possibly making the reservoir huge. */
      if (!ReservoirDeposit(reservoir, &base, &size))
        goto allDeposited;
    }
  }

  /* Just in case the shenanigans with the reservoir mucked this up. */
  AVER(limit == AddrAdd(base, size));

  /* Add the freed address space back into the freeCBS so that ArenaAlloc
     can find it again. */
  {
    CBS zoneCBS;
    RangeStruct rangeStruct;
    RangeInit(&rangeStruct, base, limit);
    /* FIXME: Multi-zone frees should go straight to freeCBS. */
    AVER(AddrZone(arena, base) == AddrZone(arena, AddrAdd(base, AddrOffset(base, limit) - 1)));
    AVER(AddrOffset(base, limit) <= (Size)1 << ArenaZoneShift(arena));
    zoneCBS = &arena->zoneCBS[AddrZone(arena, base)];
    res = CBSInsert(&rangeStruct, zoneCBS, &rangeStruct);
    if (res != ResOK) {
      /* The CBS's MFS doesn't have enough space to describe the free memory.
         Give it some of the memory we're about to free and try again. */
      Tract tract = TractOfBaseAddr(arena, base);
      Pool mfs = &arena->cbsBlockPoolStruct.poolStruct;
      AVER(size >= ArenaAlign(arena));
      TractFinish(tract);
      TractInit(tract, mfs, base);
      MFSExtend(mfs, base, ArenaAlign(arena));
      base = AddrAdd(base, ArenaAlign(arena)); /* FIXME: = all chunk's pagesizes */
      size -= ArenaAlign(arena);
      if (size == 0)
        goto allTransferred;
      RangeInit(&rangeStruct, base, limit);
      res = CBSInsert(&rangeStruct, &arena->freeCBS, &rangeStruct);
      AVER(res == ResOK);
      /* If this fails, we lose some address space forever. */
    }
  }

  AVER(limit == AddrAdd(base, size));

  (*arena->class->free)(base, size, pool);

  /* Freeing memory might create spare pages, but not more than this. */
  CHECKL(arena->spareCommitted <= arena->spareCommitLimit);

allTransferred:
allDeposited:
  EVENT3(ArenaFree, arena, wholeBase, wholeSize);
  return;
}


Size ArenaReserved(Arena arena)
{
  AVERT(Arena, arena);
  return (*arena->class->reserved)(arena);
}

Size ArenaCommitted(Arena arena)
{
  AVERT(Arena, arena);
  return arena->committed;
}

Size ArenaSpareCommitted(Arena arena)
{
  AVERT(Arena, arena);
  return arena->spareCommitted;
}

Size ArenaSpareCommitLimit(Arena arena)
{
  AVERT(Arena, arena);
  return arena->spareCommitLimit;
}

void ArenaSetSpareCommitLimit(Arena arena, Size limit)
{
  AVERT(Arena, arena);
  /* Can't check limit, as all possible values are allowed. */

  arena->spareCommitLimit = limit;
  if (arena->spareCommitLimit < arena->spareCommitted) {
    Size excess = arena->spareCommitted - arena->spareCommitLimit;
    (void)arena->class->purgeSpare(arena, excess);
  }

  EVENT2(SpareCommitLimitSet, arena, limit);
  return;
}

/* Used by arenas which don't use spare committed memory */
Size ArenaNoPurgeSpare(Arena arena, Size size)
{
  AVERT(Arena, arena);
  UNUSED(size);
  return 0;
}


Res ArenaNoGrow(Arena arena, SegPref pref, Size size)
{
  AVERT(Arena, arena);
  AVERT(SegPref, pref);
  UNUSED(size);
  return ResRESOURCE;
}


Size ArenaCommitLimit(Arena arena)
{
  AVERT(Arena, arena);
  return arena->commitLimit;
}

Res ArenaSetCommitLimit(Arena arena, Size limit)
{
  Size committed;
  Res res;

  AVERT(Arena, arena);
  AVER(ArenaCommitted(arena) <= arena->commitLimit);

  committed = ArenaCommitted(arena);
  if (limit < committed) {
    /* Attempt to set the limit below current committed */
    if (limit >= committed - arena->spareCommitted) {
      Size excess = committed - limit;
      (void)arena->class->purgeSpare(arena, excess);
      AVER(limit >= ArenaCommitted(arena));
      arena->commitLimit = limit;
      res = ResOK;
    } else {
      res = ResFAIL;
    }
  } else {
    arena->commitLimit = limit;
    res = ResOK;
  }
  EVENT3(CommitLimitSet, arena, limit, (res == ResOK));
  return res;
}


/* ArenaAvail -- return available memory in the arena */

Size ArenaAvail(Arena arena)
{
  Size sSwap;

  sSwap = ArenaReserved(arena);
  if (sSwap > arena->commitLimit)
    sSwap = arena->commitLimit;

  /* TODO: sSwap should take into account the amount of backing store
     available to supply the arena with memory.  This would be the amount
     available in the paging file, which is possibly the amount of free
     disk space in some circumstances.  We'd have to see whether we can get
     this information from the operating system.  It also depends on the
     arena class, of course. */

  return sSwap - arena->committed + arena->spareCommitted;
}


/* ArenaExtend -- Add a new chunk in the arena */

Res ArenaExtend(Arena arena, Addr base, Size size)
{
  Res res;

  AVERT(Arena, arena);
  AVER(base != (Addr)0);
  AVER(size > 0);

  res = (*arena->class->extend)(arena, base, size);
  if (res != ResOK)
    return res;

  EVENT3(ArenaExtend, arena, base, size);
  return ResOK;
}


/* ArenaNoExtend -- fail to extend the arena by a chunk */

Res ArenaNoExtend(Arena arena, Addr base, Size size)
{
  AVERT(Arena, arena);
  AVER(base != (Addr)0);
  AVER(size > (Size)0);

  NOTREACHED;
  return ResUNIMPL;
}


/* ArenaCompact -- respond (or not) to trace reclaim */

void ArenaCompact(Arena arena, Trace trace)
{
  AVERT(Arena, arena);
  AVERT(Trace, trace);
  (*arena->class->compact)(arena, trace);
}

static void ArenaTrivCompact(Arena arena, Trace trace)
{
  UNUSED(arena);
  UNUSED(trace);
  return;
}


/* Has Addr */

Bool ArenaHasAddr(Arena arena, Addr addr)
{
  Seg seg;

  AVERT(Arena, arena);
  return SegOfAddr(&seg, arena, addr);
}


/* ArenaAddrObject -- find client pointer to object containing addr
 * See job003589.
 */

Res ArenaAddrObject(Addr *pReturn, Arena arena, Addr addr)
{
  Seg seg;
  Pool pool;

  AVER(pReturn != NULL);
  AVERT(Arena, arena);
  
  if (!SegOfAddr(&seg, arena, addr)) {
    return ResFAIL;
  }
  pool = SegPool(seg);
  return PoolAddrObject(pReturn, pool, seg, addr);
}


/* C. COPYRIGHT AND LICENSE
 *
 * Copyright (C) 2001-2002 Ravenbrook Limited <http://www.ravenbrook.com/>.
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
