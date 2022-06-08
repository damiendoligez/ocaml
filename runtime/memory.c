/**************************************************************************/
/*                                                                        */
/*                                 OCaml                                  */
/*                                                                        */
/*              Damien Doligez, projet Para, INRIA Rocquencourt           */
/*                                                                        */
/*   Copyright 1996 Institut National de Recherche en Informatique et     */
/*     en Automatique.                                                    */
/*                                                                        */
/*   All rights reserved.  This file is distributed under the terms of    */
/*   the GNU Lesser General Public License version 2.1, with the          */
/*   special exception on linking described in the file LICENSE.          */
/*                                                                        */
/**************************************************************************/

#define CAML_INTERNALS

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include "caml/address_class.h"
#include "caml/config.h"
#include "caml/fail.h"
#include "caml/freelist.h"
#include "caml/gc.h"
#include "caml/gc_ctrl.h"
#include "caml/major_gc.h"
#include "caml/memory.h"
#include "caml/major_gc.h"
#include "caml/minor_gc.h"
#include "caml/misc.h"
#include "caml/mlvalues.h"
#include "caml/signals.h"
#include "caml/memprof.h"
#include "caml/eventlog.h"

int caml_huge_fallback_count = 0;
/* Number of times that mmapping big pages fails and we fell back to small
   pages. This counter is available to the program through
   [Gc.huge_fallback_count].
*/

uintnat caml_use_huge_pages = 0;
/* True iff the program allocates heap chunks by mmapping huge pages.
   This is set when parsing [OCAMLRUNPARAM] and must stay constant
   after that.
*/

extern uintnat caml_percent_free;                   /* major_gc.c */

/* Page table management */

#define Page(p) ((uintnat) (p) >> Page_log)
#define Page_mask ((~(uintnat)0) << Page_log)

#ifdef ARCH_SIXTYFOUR

/* 64-bit implementation:
   The page table is represented sparsely as a hash table
   with linear probing */

struct page_table {
  mlsize_t size;                /* size == 1 << (wordsize - shift) */
  int shift;
  mlsize_t mask;                /* mask == size - 1 */
  mlsize_t occupancy;
  uintnat * entries;            /* [size]  */
};

static struct page_table caml_page_table;

/* Page table entries are the logical 'or' of
   - the key: address of a page (low Page_log bits = 0)
   - the data: a 8-bit integer */

#define Page_entry_matches(entry,addr) \
  ((((entry) ^ (addr)) & Page_mask) == 0)

/* Multiplicative Fibonacci hashing
   (Knuth, TAOCP vol 3, section 6.4, page 518).
   HASH_FACTOR is (sqrt(5) - 1) / 2 * 2^wordsize. */
#ifdef ARCH_SIXTYFOUR
#define HASH_FACTOR 11400714819323198486UL
#else
#define HASH_FACTOR 2654435769UL
#endif
#define Hash(v) (((v) * HASH_FACTOR) >> caml_page_table.shift)

int caml_page_table_lookup(void * addr)
{
  uintnat h, e;

  h = Hash(Page(addr));
  /* The first hit is almost always successful, so optimize for this case */
  e = caml_page_table.entries[h];
  if (Page_entry_matches(e, (uintnat)addr)) return e & 0xFF;
  while(1) {
    if (e == 0) return 0;
    h = (h + 1) & caml_page_table.mask;
    e = caml_page_table.entries[h];
    if (Page_entry_matches(e, (uintnat)addr)) return e & 0xFF;
  }
}

int caml_page_table_initialize(mlsize_t bytesize)
{
  uintnat pagesize = Page(bytesize);

  caml_page_table.size = 1;
  caml_page_table.shift = 8 * sizeof(uintnat);
  /* Aim for initial load factor between 1/4 and 1/2 */
  while (caml_page_table.size < 2 * pagesize) {
    caml_page_table.size <<= 1;
    caml_page_table.shift -= 1;
  }
  caml_page_table.mask = caml_page_table.size - 1;
  caml_page_table.occupancy = 0;
  caml_page_table.entries =
    caml_stat_calloc_noexc(caml_page_table.size, sizeof(uintnat));
  if (caml_page_table.entries == NULL)
    return -1;
  else
    return 0;
}

static int caml_page_table_resize(void)
{
  struct page_table old = caml_page_table;
  uintnat * new_entries;
  uintnat i, h;

  caml_gc_message (0x08, "Growing page table to %"
                   ARCH_INTNAT_PRINTF_FORMAT "u entries\n",
                   caml_page_table.size);

  new_entries = caml_stat_calloc_noexc(2 * old.size, sizeof(uintnat));
  if (new_entries == NULL) {
    caml_gc_message (0x08, "No room for growing page table\n");
    return -1;
  }

  caml_page_table.size = 2 * old.size;
  caml_page_table.shift = old.shift - 1;
  caml_page_table.mask = caml_page_table.size - 1;
  caml_page_table.occupancy = old.occupancy;
  caml_page_table.entries = new_entries;

  for (i = 0; i < old.size; i++) {
    uintnat e = old.entries[i];
    if (e == 0) continue;
    h = Hash(Page(e));
    while (caml_page_table.entries[h] != 0)
      h = (h + 1) & caml_page_table.mask;
    caml_page_table.entries[h] = e;
  }

  caml_stat_free(old.entries);
  return 0;
}

static int caml_page_table_modify(uintnat page, int toclear, int toset)
{
  uintnat h;

  CAMLassert ((page & ~Page_mask) == 0);

  /* Resize to keep load factor below 1/2 */
  if (caml_page_table.occupancy * 2 >= caml_page_table.size) {
    if (caml_page_table_resize() != 0) return -1;
  }
  h = Hash(Page(page));
  while (1) {
    if (caml_page_table.entries[h] == 0) {
      caml_page_table.entries[h] = page | toset;
      caml_page_table.occupancy++;
      break;
    }
    if (Page_entry_matches(caml_page_table.entries[h], page)) {
      caml_page_table.entries[h] =
        (caml_page_table.entries[h] & ~toclear) | toset;
      break;
    }
    h = (h + 1) & caml_page_table.mask;
  }
  return 0;
}

#else

/* 32-bit implementation:
   The page table is represented as a 2-level array of unsigned char */

CAMLexport unsigned char * caml_page_table[Pagetable1_size];
static unsigned char caml_page_table_empty[Pagetable2_size] = { 0, };

int caml_page_table_initialize(mlsize_t bytesize)
{
  int i;
  for (i = 0; i < Pagetable1_size; i++)
    caml_page_table[i] = caml_page_table_empty;
  return 0;
}

static int caml_page_table_modify(uintnat page, int toclear, int toset)
{
  uintnat i = Pagetable_index1(page);
  uintnat j = Pagetable_index2(page);

  if (caml_page_table[i] == caml_page_table_empty) {
    unsigned char * new_tbl = caml_stat_calloc_noexc(Pagetable2_size, 1);
    if (new_tbl == 0) return -1;
    caml_page_table[i] = new_tbl;
  }
  caml_page_table[i][j] = (caml_page_table[i][j] & ~toclear) | toset;
  return 0;
}

#endif

int caml_page_table_add(int kind, void * start, void * end)
{
  uintnat pstart = (uintnat) start & Page_mask;
  uintnat pend = ((uintnat) end - 1) & Page_mask;
  uintnat p;

  for (p = pstart; p <= pend; p += Page_size)
    if (caml_page_table_modify(p, 0, kind) != 0) return -1;
  return 0;
}

int caml_page_table_remove(int kind, void * start, void * end)
{
  uintnat pstart = (uintnat) start & Page_mask;
  uintnat pend = ((uintnat) end - 1) & Page_mask;
  uintnat p;

  for (p = pstart; p <= pend; p += Page_size)
    if (caml_page_table_modify(p, kind, 0) != 0) return -1;
  return 0;
}

/* Allocate a block of the requested size, to be passed to
   [caml_add_to_heap] later.
   [request] will be rounded up to some implementation-dependent size.
   The caller must use [Chunk_size] on the result to recover the actual
   size.
   Return NULL if the request cannot be satisfied. The returned pointer
   is a hp, but the header (and the contents) must be initialized by the
   caller.
*/
char *caml_alloc_for_heap (asize_t request)
{
  if (caml_use_huge_pages){
#ifdef HAS_HUGE_PAGES
    uintnat size = Round_mmap_size (sizeof (heap_chunk_head) + request);
    void *block;
    char *mem;
    block = mmap (NULL, size, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (block == MAP_FAILED) return NULL;
    mem = (char *) block + sizeof (heap_chunk_head);
    Chunk_size (mem) = size - sizeof (heap_chunk_head);
    Chunk_block (mem) = block;
    Chunk_redarken_start(mem) = (value*)(mem + Chunk_size(mem));
    Chunk_redarken_end(mem) = (value*)mem;
    return mem;
#else
    return NULL;
#endif
  }else{
    char *mem;
    void *block;

    request = ((request + Page_size - 1) >> Page_log) << Page_log;
    mem = caml_stat_alloc_aligned_noexc (request + sizeof (heap_chunk_head),
                                         sizeof (heap_chunk_head), &block);
    if (mem == NULL) return NULL;
    mem += sizeof (heap_chunk_head);
    Chunk_size (mem) = request;
    Chunk_block (mem) = block;
    Chunk_redarken_start(mem) = (value*)(mem + Chunk_size(mem));
    Chunk_redarken_end(mem) = (value*)mem;
    return mem;
  }
}

/* Use this function to free a block allocated with [caml_alloc_for_heap]
   if you don't add it with [caml_add_to_heap].
*/
void caml_free_for_heap (char *mem)
{
  if (caml_use_huge_pages){
#ifdef HAS_HUGE_PAGES
    munmap (Chunk_block (mem), Chunk_size (mem) + sizeof (heap_chunk_head));
#else
    CAMLassert (0);
#endif
  }else{
    caml_stat_free (Chunk_block (mem));
  }
}

/* Take a chunk of memory as argument, which must be the result of a
   call to [caml_alloc_for_heap], and insert it into the heap chaining.
   The contents of the chunk must be a sequence of valid blocks and
   fragments: no space between blocks and no trailing garbage.  If
   some blocks are blue, they must be added to the free list by the
   caller.  All other blocks must have the color [caml_allocation_color(m)].
   The caller must update [caml_allocated_words] if applicable.
   Return value: 0 if no error; -1 in case of error.

   See also: caml_compact_heap, which duplicates most of this function.
*/
int caml_add_to_heap (char *m)
{
#ifdef DEBUG
  /* Should check the contents of the block. */
#endif /* DEBUG */

  caml_gc_message (0x04, "Growing heap to %"
                   ARCH_INTNAT_PRINTF_FORMAT "uk bytes\n",
     (Bsize_wsize (Caml_state->stat_heap_wsz) + Chunk_size (m)) / 1024);

  /* Register block in page table */
  if (caml_page_table_add(In_heap, m, m + Chunk_size(m)) != 0)
    return -1;

  /* Chain this heap chunk. */
  {
    char **last = &caml_heap_start;
    char *cur = *last;

    while (cur != NULL && cur < m){
      last = &(Chunk_next (cur));
      cur = *last;
    }
    Chunk_next (m) = cur;
    *last = m;

    ++ Caml_state->stat_heap_chunks;
  }

  Caml_state->stat_heap_wsz += Wsize_bsize (Chunk_size (m));
  if (Caml_state->stat_heap_wsz > Caml_state->stat_top_heap_wsz){
    Caml_state->stat_top_heap_wsz = Caml_state->stat_heap_wsz;
  }
  return 0;
}

/* Allocate more memory from malloc for the heap.
   Return a blue block of at least the requested size.
   The blue block is chained to a sequence of blue blocks (through their
   field 0); the last block of the chain is pointed by field 1 of the
   first.  There may be a fragment after the last block.
   The caller must insert the blocks into the free list.
   [request] is a number of words and must be less than or equal
   to [Max_wosize].
   Return NULL when out of memory.
*/
static value *expand_heap (mlsize_t request)
{
  /* these point to headers, but we do arithmetic on them, hence [value *]. */
  value *mem, *hp, *prev;
  asize_t over_request, malloc_request, remain;

  CAMLassert (request <= Max_wosize);
  over_request = request + request / 100 * caml_percent_free;
  malloc_request = caml_clip_heap_chunk_wsz (over_request);
  mem = (value *) caml_alloc_for_heap (Bsize_wsize (malloc_request));
  if (mem == NULL){
    caml_gc_message (0x04, "No room for growing heap\n");
    return NULL;
  }
  remain = Wsize_bsize (Chunk_size (mem));
  prev = hp = mem;
  /* FIXME find a way to do this with a call to caml_make_free_blocks */
  while (Wosize_whsize (remain) > Max_wosize){
    Hd_hp (hp) = Make_header (Max_wosize, 0, Caml_blue);
#ifdef DEBUG
    caml_set_fields (Val_hp (hp), 0, Debug_free_major);
#endif
    hp += Whsize_wosize (Max_wosize);
    remain -= Whsize_wosize (Max_wosize);
    Field (Val_hp (mem), 1) = Field (Val_hp (prev), 0) = Val_hp (hp);
    prev = hp;
  }
  if (remain > 1){
    Hd_hp (hp) = Make_header (Wosize_whsize (remain), 0, Caml_blue);
#ifdef DEBUG
    caml_set_fields (Val_hp (hp), 0, Debug_free_major);
#endif
    Field (Val_hp (mem), 1) = Field (Val_hp (prev), 0) = Val_hp (hp);
    Field (Val_hp (hp), 0) = (value) NULL;
  }else{
    Field (Val_hp (prev), 0) = (value) NULL;
    if (remain == 1) {
      Hd_hp (hp) = Make_header (0, 0, Caml_white);
    }
  }
  CAMLassert (Wosize_hp (mem) >= request);
  if (caml_add_to_heap ((char *) mem) != 0){
    caml_free_for_heap ((char *) mem);
    return NULL;
  }
  return Op_hp (mem);
}

/* Remove the heap chunk [chunk] from the heap and give the memory back
   to [free].
*/
void caml_shrink_heap (char *chunk)
{
  char **cp;

  /* Never deallocate the first chunk, because caml_heap_start is both the
     first block and the base address for page numbers, and we don't
     want to shift the page table, it's too messy (see above).
     It will never happen anyway, because of the way compaction works.
     (see compact.c)
     XXX FIXME this has become false with the fix to PR#5389 (see compact.c)
  */
  if (chunk == caml_heap_start) return;

  Caml_state->stat_heap_wsz -= Wsize_bsize (Chunk_size (chunk));
  caml_gc_message (0x04, "Shrinking heap to %"
                   ARCH_INTNAT_PRINTF_FORMAT "dk words\n",
                   Caml_state->stat_heap_wsz / 1024);

#ifdef DEBUG
  {
    mlsize_t i;
    for (i = 0; i < Wsize_bsize (Chunk_size (chunk)); i++){
      ((value *) chunk) [i] = Debug_free_shrink;
    }
  }
#endif

  -- Caml_state->stat_heap_chunks;

  /* Remove [chunk] from the list of chunks. */
  cp = &caml_heap_start;
  while (*cp != chunk) cp = &(Chunk_next (*cp));
  *cp = Chunk_next (chunk);

  /* Remove the pages of [chunk] from the page table. */
  caml_page_table_remove(In_heap, chunk, chunk + Chunk_size (chunk));

  /* Free the [malloc] block that contains [chunk]. */
  caml_free_for_heap (chunk);
}

CAMLexport color_t caml_allocation_color (void *hp)
{
  if (caml_gc_phase == Phase_mark || caml_gc_phase == Phase_clean ||
      (caml_gc_phase == Phase_sweep && (char *)hp >= (char *)caml_gc_sweep_hp)){
    return Caml_black;
  }else{
    CAMLassert (caml_gc_phase == Phase_idle
            || (caml_gc_phase == Phase_sweep
                && (char *)hp < (char *)caml_gc_sweep_hp));
    return Caml_white;
  }
}

Caml_inline value caml_alloc_shr_aux (mlsize_t wosize, tag_t tag, int track,
                                      uintnat profinfo)
{
  header_t *hp;
  value *new_block;

  if (wosize > Max_wosize) return 0;
  CAML_EV_ALLOC(wosize);
  hp = caml_fl_allocate (wosize);
  if (hp == NULL){
    new_block = expand_heap (wosize);
    if (new_block == NULL) return 0;
    caml_fl_add_blocks ((value) new_block);
    hp = caml_fl_allocate (wosize);
  }

  CAMLassert (Is_in_heap (Val_hp (hp)));

  /* Inline expansion of caml_allocation_color. */
  if (caml_gc_phase == Phase_mark || caml_gc_phase == Phase_clean ||
      (caml_gc_phase == Phase_sweep && (char *)hp >= (char *)caml_gc_sweep_hp)){
    Hd_hp (hp) = Make_header_with_profinfo (wosize, tag, Caml_black, profinfo);
  }else{
    CAMLassert (caml_gc_phase == Phase_idle
            || (caml_gc_phase == Phase_sweep
                && (char *)hp < (char *)caml_gc_sweep_hp));
    Hd_hp (hp) = Make_header_with_profinfo (wosize, tag, Caml_white, profinfo);
  }
  CAMLassert (Hd_hp (hp)
    == Make_header_with_profinfo (wosize, tag, caml_allocation_color (hp),
                                  profinfo));
  caml_allocated_words += Whsize_wosize (wosize);
  if (caml_allocated_words > Caml_state->minor_heap_wsz){
    CAML_EV_COUNTER (EV_C_REQUEST_MAJOR_ALLOC_SHR, 1);
    caml_request_major_slice ();
  }
#ifdef DEBUG
  {
    uintnat i;
    for (i = 0; i < wosize; i++){
      Field (Val_hp (hp), i) = Debug_uninit_major;
    }
  }
#endif
  if(track)
    caml_memprof_track_alloc_shr(Val_hp (hp));
  return Val_hp (hp);
}

Caml_inline value check_oom(value v)
{
  if (v == 0) {
    if (Caml_state->in_minor_collection)
      caml_fatal_error ("out of memory");
    else
      caml_raise_out_of_memory ();
  }
  return v;
}

CAMLexport value caml_alloc_shr_with_profinfo (mlsize_t wosize, tag_t tag,
                                               intnat profinfo)
{
  return check_oom(caml_alloc_shr_aux(wosize, tag, 1, profinfo));
}

CAMLexport value caml_alloc_shr_for_minor_gc (mlsize_t wosize,
                                              tag_t tag, header_t old_hd)
{
  return check_oom(caml_alloc_shr_aux(wosize, tag, 0, Profinfo_hd(old_hd)));
}

CAMLexport value caml_alloc_shr (mlsize_t wosize, tag_t tag)
{
  return caml_alloc_shr_with_profinfo(wosize, tag, NO_PROFINFO);
}

CAMLexport value caml_alloc_shr_no_track_noexc (mlsize_t wosize, tag_t tag)
{
  return caml_alloc_shr_aux(wosize, tag, 0, NO_PROFINFO);
}

/* Dependent memory is all memory blocks allocated out of the heap
   that depend on the GC (and finalizers) for deallocation.
   For the GC to take dependent memory into account when computing
   its automatic speed setting,
   you must call [caml_alloc_dependent_memory] when you allocate some
   dependent memory, and [caml_free_dependent_memory] when you
   free it.  In both cases, you pass as argument the size (in bytes)
   of the block being allocated or freed.
*/
CAMLexport void caml_alloc_dependent_memory (mlsize_t nbytes)
{
  caml_dependent_size += nbytes / sizeof (value);
  caml_dependent_allocated += nbytes / sizeof (value);
}

CAMLexport void caml_free_dependent_memory (mlsize_t nbytes)
{
  if (caml_dependent_size < nbytes / sizeof (value)){
    caml_dependent_size = 0;
  }else{
    caml_dependent_size -= nbytes / sizeof (value);
  }
}

/* Use this function to tell the major GC to speed up when you use
   finalized blocks to automatically deallocate resources (other
   than memory). The GC will do at least one cycle every [max]
   allocated resources; [res] is the number of resources allocated
   this time.
   Note that only [res/max] is relevant.  The units (and kind of
   resource) can change between calls to [caml_adjust_gc_speed].
*/
CAMLexport void caml_adjust_gc_speed (mlsize_t res, mlsize_t max)
{
  if (max == 0) max = 1;
  if (res > max) res = max;
  caml_extra_heap_resources += (double) res / (double) max;
  if (caml_extra_heap_resources > 1.0){
    CAML_EV_COUNTER (EV_C_REQUEST_MAJOR_ADJUST_GC_SPEED, 1);
    caml_extra_heap_resources = 1.0;
    caml_request_major_slice ();
  }
}

/* You must use [caml_initialize] to store the initial value in a field of
   a shared block, unless you are sure the value is not a young block.
   A block value [v] is a shared block if and only if [Is_in_heap (v)]
   is true.
*/
/* [caml_initialize] never calls the GC, so you may call it while a block is
   unfinished (i.e. just after a call to [caml_alloc_shr].) */
/* PR#6084 workaround: define it as a weak symbol */
CAMLexport CAMLweakdef void caml_initialize (value *fp, value val)
{
  CAMLassert(Is_in_heap_or_young(fp));
  *fp = val;
  if (!Is_young((value)fp) && Is_block (val) && Is_young (val)) {
    add_to_ref_table (Caml_state->ref_table, fp);
  }
}

#define MODIFY_CACHE_BITS 10
#define MODIFY_CACHE_SIZE (1 << MODIFY_CACHE_BITS)
#define MODIFY_CACHE_SHIFT (8 * sizeof (uintnat) - MODIFY_CACHE_BITS)
#define MODIFY_CACHE_HASH_FACTOR 11400714819323198485UL
#define LOG_WORD_SIZE (sizeof (uintnat) / 4 + 1)

struct modify_cache_entry {
  value *field_pointer;
  int in_ref_table;
};

static struct modify_cache_entry modify_cache [MODIFY_CACHE_SIZE];

static inline uintnat modify_hash (value *fp)
{
  return (((uintnat) fp /*>> LOG_WORD_SIZE*/)
          * MODIFY_CACHE_HASH_FACTOR)
         >> MODIFY_CACHE_SHIFT;
}

void caml_modify_batch (void)
{

  /* The write barrier implemented by [caml_modify] checks for the
     following two conditions and takes appropriate action:
     1- creation of a pointer from the major heap to the minor heap
        --> add [fp] to the remembered set
     2- overwriting of a pointer from the major heap to the major heap that
        was already present at the start of the GC cycle,
        while the GC is in the marking phase
        --> call [caml_darken] on the overwritten pointer so that the
            major GC treats it as an additional root.

     The logic implemented below is duplicated (without the cache)
     in caml_array_fill to
     avoid repeated calls to caml_modify and repeated tests on the
     values.  Don't forget to update caml_array_fill if the logic
     below changes!
  */

  intnat i, index;
  value *fp;
  value old, val;
  uintnat h;

  CAML_EV_BEGIN(EV_MODIFY_BATCH);
  index =
    (intnat) (Caml_state->modify_log_index / sizeof (struct modify_log_entry));
  for (i = CAML_MODIFY_LOG_SIZE - 1; i >= index; i--){
    fp = Caml_state->modify_log[i].field_pointer;
    if (Is_young((value)fp)){
      /* The modified object resides in the minor heap.
         Conditions 1 and 2 cannot occur. */
      continue;
    } else {
      /* The modified object resides in the major heap. */
      CAMLassert(Is_in_heap(fp));
      h = modify_hash (fp);
      CAMLassert (fp != NULL);
      if (modify_cache[h].field_pointer == fp){
        CAML_EV_COUNTER (EV_C_CAML_MODIFY_CACHE_HIT, 1);
#ifdef DEBUG
        fprintf (stderr, "hit %04lx fp=%p\n", h, fp);
#endif
        /* Writing again to an already-modified field:
           condition 2 cannot occur. */
        if (!modify_cache[h].in_ref_table){
          /* Check for condition 1. */
          val = *fp;
          if (Is_block(val) && Is_young(val)) {
            add_to_ref_table (Caml_state->ref_table, fp);
            modify_cache[h].in_ref_table = 1;
          }
        }
      }else{
        CAML_EV_COUNTER (EV_C_CAML_MODIFY_CACHE_MISS, 1);
#ifdef DEBUG
        CAMLassert (fp != NULL);
        fprintf (stderr, "miss %04lx cache=%p fp=%p\n", h, modify_cache[h].field_pointer, fp);
#endif
        modify_cache[h].field_pointer = fp;
        modify_cache[h].in_ref_table = 0;
        old = Caml_state->modify_log[i].old_value;
        if (Is_block(old)) {
          /* If [old] is a pointer into the minor heap:
             - Condition 2 cannot occur.
             - Condition 1 can only occur when overwriting a non-minor
               pointer with a minor pointer. The batch entry for that
               write will have added this field to [ref_table] so we
               don't need to do it here.
          */
          if (Is_young(old)){
            continue;
          }
          /* Here, [old] can be a pointer within the major heap.
             Check for condition 2. */
          if (caml_gc_phase == Phase_mark) caml_darken(old, NULL);
        }
        /* Check for condition 1. */
        val = *fp;
        if (Is_block(val) && Is_young(val)) {
          add_to_ref_table (Caml_state->ref_table, fp);
          modify_cache[h].in_ref_table = 1;
        }
      }
    }
  }
  Caml_state->modify_log_index =
    CAML_MODIFY_LOG_SIZE * sizeof (struct modify_log_entry);
  CAML_EV_END(EV_MODIFY_BATCH);
}

/* You must use [caml_modify] to change a field of an existing shared block,
   unless you are sure the value being overwritten is not a shared block and
   the value being written is not a young block. */
/* [caml_modify] never calls the GC. */
/* [caml_modify] can also be used to do assignment on data structures that are
   in the minor heap instead of in the major heap.  In this case, it
   is a bit slower than simple assignment.
   In particular, you can use [caml_modify] when you don't know whether the
   block being changed is in the minor heap or the major heap. */
/* PR#6084 workaround: define it as a weak symbol */

CAMLexport CAMLweakdef void caml_modify (value *fp, value val)
{
  uintnat i;

  if (Caml_state->modify_log_index == 0){
    caml_modify_batch ();
  }
  Caml_state->modify_log_index -= sizeof (struct modify_log_entry);
  i = Caml_state->modify_log_index / sizeof (struct modify_log_entry);
  Caml_state->modify_log[i].field_pointer = fp;
  Caml_state->modify_log[i].old_value = *fp;
  *fp = val;
}

void caml_modify_flush_cache (void)
{
  int i;
#ifdef DEBUG
  fprintf (stderr, "caml_modify_cache_flush\n");
#endif
  for (i = 0; i < MODIFY_CACHE_SIZE; i++){
    modify_cache[i].field_pointer = NULL;
  }
}

void caml_init_modify (void)
{
  Caml_state->modify_log =
    caml_stat_alloc_noexc (CAML_MODIFY_LOG_SIZE
                           * sizeof (struct modify_log_entry));
  if(Caml_state->modify_log == NULL)
    caml_fatal_error("not enough memory for the modify log");
#if 0
  #define Debug_tag(x) (INT64_LITERAL(0xD700D7D7D700D6D7u) \
                      | ((uintnat) (x) << 16) \
                      | ((uintnat) (x) << 48))
  {
    int i;
    for (i = 0; i < CAML_MODIFY_LOG_SIZE; i++){
      Caml_state->modify_log[i].field_pointer = (void *) Debug_tag(0x20);
      Caml_state->modify_log[i].old_value = Debug_tag(0x21);
      /* XXX FIXME move these constants to misc.h */
    }
  }
#endif
  Caml_state->modify_log_index =
    CAML_MODIFY_LOG_SIZE * sizeof (struct modify_log_entry);
  caml_modify_flush_cache ();
}

/* Global memory pool.

   The pool is structured as a ring of blocks, where each block's header
   contains two links: to the previous and to the next block. The data
   structure allows for insertions and removals of blocks in constant time,
   given that a pointer to the operated block is provided.

   Initially, the pool contains a single block -- a pivot with no data, the
   guaranteed existence of which makes for a more concise implementation.

   The API functions that operate on the pool receive not pointers to the
   block's header, but rather pointers to the block's "data" field. This
   behaviour is required to maintain compatibility with the interfaces of
   [malloc], [realloc], and [free] family of functions, as well as to hide
   the implementation from the user.
*/

/* A type with the most strict alignment requirements */
union max_align {
  char c;
  short s;
  long l;
  int i;
  float f;
  double d;
  void *v;
  void (*q)(void);
};

struct pool_block {
#ifdef DEBUG
  intnat magic;
#endif
  struct pool_block *next;
  struct pool_block *prev;
  /* Use C99's flexible array types if possible */
#if (__STDC_VERSION__ >= 199901L)
  union max_align data[];  /* not allocated, used for alignment purposes */
#else
  union max_align data[1];
#endif
};

#if (__STDC_VERSION__ >= 199901L)
#define SIZEOF_POOL_BLOCK sizeof(struct pool_block)
#else
#define SIZEOF_POOL_BLOCK offsetof(struct pool_block, data)
#endif

static struct pool_block *pool = NULL;


/* Returns a pointer to the block header, given a pointer to "data" */
static struct pool_block* get_pool_block(caml_stat_block b)
{
  if (b == NULL)
    return NULL;

  else {
    struct pool_block *pb =
      (struct pool_block*)(((char*)b) - SIZEOF_POOL_BLOCK);
#ifdef DEBUG
    CAMLassert(pb->magic == Debug_pool_magic);
#endif
    return pb;
  }
}

CAMLexport void caml_stat_create_pool(void)
{
  if (pool == NULL) {
    pool = malloc(SIZEOF_POOL_BLOCK);
    if (pool == NULL)
      caml_fatal_error("out of memory");
#ifdef DEBUG
    pool->magic = Debug_pool_magic;
#endif
    pool->next = pool;
    pool->prev = pool;
  }
}

CAMLexport void caml_stat_destroy_pool(void)
{
  if (pool != NULL) {
    pool->prev->next = NULL;
    while (pool != NULL) {
      struct pool_block *next = pool->next;
      free(pool);
      pool = next;
    }
    pool = NULL;
  }
}

/* [sz] and [modulo] are numbers of bytes */
CAMLexport void* caml_stat_alloc_aligned_noexc(asize_t sz, int modulo,
                                               caml_stat_block *b)
{
  char *raw_mem;
  uintnat aligned_mem;
  CAMLassert (0 <= modulo && modulo < Page_size);
  raw_mem = (char *) caml_stat_alloc_noexc(sz + Page_size);
  if (raw_mem == NULL) return NULL;
  *b = raw_mem;
  raw_mem += modulo;                /* Address to be aligned */
  aligned_mem = (((uintnat) raw_mem / Page_size + 1) * Page_size);
#ifdef DEBUG
  {
    uintnat *p;
    uintnat *p0 = (void *) *b;
    uintnat *p1 = (void *) (aligned_mem - modulo);
    uintnat *p2 = (void *) (aligned_mem - modulo + sz);
    uintnat *p3 = (void *) ((char *) *b + sz + Page_size);
    for (p = p0; p < p1; p++) *p = Debug_filler_align;
    for (p = p1; p < p2; p++) *p = Debug_uninit_align;
    for (p = p2; p < p3; p++) *p = Debug_filler_align;
  }
#endif
  return (char *) (aligned_mem - modulo);
}

/* [sz] and [modulo] are numbers of bytes */
CAMLexport void* caml_stat_alloc_aligned(asize_t sz, int modulo,
                                         caml_stat_block *b)
{
  void *result = caml_stat_alloc_aligned_noexc(sz, modulo, b);
  /* malloc() may return NULL if size is 0 */
  if ((result == NULL) && (sz != 0))
    caml_raise_out_of_memory();
  return result;
}

/* [sz] is a number of bytes */
CAMLexport caml_stat_block caml_stat_alloc_noexc(asize_t sz)
{
  /* Backward compatibility mode */
  if (pool == NULL)
    return malloc(sz);
  else {
    struct pool_block *pb = malloc(sz + SIZEOF_POOL_BLOCK);
    if (pb == NULL) return NULL;
#ifdef DEBUG
    memset(&(pb->data), Debug_uninit_stat, sz);
    pb->magic = Debug_pool_magic;
#endif

    /* Linking the block into the ring */
    pb->next = pool->next;
    pb->prev = pool;
    pool->next->prev = pb;
    pool->next = pb;

    return &(pb->data);
  }
}

/* [sz] is a number of bytes */
CAMLexport caml_stat_block caml_stat_alloc(asize_t sz)
{
  void *result = caml_stat_alloc_noexc(sz);
  /* malloc() may return NULL if size is 0 */
  if ((result == NULL) && (sz != 0))
    caml_raise_out_of_memory();
  return result;
}

CAMLexport void caml_stat_free(caml_stat_block b)
{
  /* Backward compatibility mode */
  if (pool == NULL)
    free(b);
  else {
    struct pool_block *pb = get_pool_block(b);
    if (pb == NULL) return;

    /* Unlinking the block from the ring */
    pb->prev->next = pb->next;
    pb->next->prev = pb->prev;

    free(pb);
  }
}

/* [sz] is a number of bytes */
CAMLexport caml_stat_block caml_stat_resize_noexc(caml_stat_block b, asize_t sz)
{
  if(b == NULL)
    return caml_stat_alloc_noexc(sz);
  /* Backward compatibility mode */
  if (pool == NULL)
    return realloc(b, sz);
  else {
    struct pool_block *pb = get_pool_block(b);
    struct pool_block *pb_new = realloc(pb, sz + SIZEOF_POOL_BLOCK);
    if (pb_new == NULL) return NULL;

    /* Relinking the new block into the ring in place of the old one */
    pb_new->prev->next = pb_new;
    pb_new->next->prev = pb_new;

    return &(pb_new->data);
  }
}

/* [sz] is a number of bytes */
CAMLexport caml_stat_block caml_stat_resize(caml_stat_block b, asize_t sz)
{
  void *result = caml_stat_resize_noexc(b, sz);
  if (result == NULL)
    caml_raise_out_of_memory();
  return result;
}

/* [sz] is a number of bytes */
CAMLexport caml_stat_block caml_stat_calloc_noexc(asize_t num, asize_t sz)
{
  uintnat total;
  if (caml_umul_overflow(sz, num, &total))
    return NULL;
  else {
    caml_stat_block result = caml_stat_alloc_noexc(total);
    if (result != NULL)
      memset(result, 0, total);
    return result;
  }
}

CAMLexport caml_stat_string caml_stat_strdup_noexc(const char *s)
{
  size_t slen = strlen(s);
  caml_stat_block result = caml_stat_alloc_noexc(slen + 1);
  if (result == NULL)
    return NULL;
  memcpy(result, s, slen + 1);
  return result;
}

CAMLexport caml_stat_string caml_stat_strdup(const char *s)
{
  caml_stat_string result = caml_stat_strdup_noexc(s);
  if (result == NULL)
    caml_raise_out_of_memory();
  return result;
}

#ifdef _WIN32

CAMLexport wchar_t * caml_stat_wcsdup(const wchar_t *s)
{
  int slen = wcslen(s);
  wchar_t* result = caml_stat_alloc((slen + 1)*sizeof(wchar_t));
  if (result == NULL)
    caml_raise_out_of_memory();
  memcpy(result, s, (slen + 1)*sizeof(wchar_t));
  return result;
}

#endif

CAMLexport caml_stat_string caml_stat_strconcat(int n, ...)
{
  va_list args;
  char *result, *p;
  size_t len = 0;
  int i;

  va_start(args, n);
  for (i = 0; i < n; i++) {
    const char *s = va_arg(args, const char*);
    len += strlen(s);
  }
  va_end(args);

  result = caml_stat_alloc(len + 1);

  va_start(args, n);
  p = result;
  for (i = 0; i < n; i++) {
    const char *s = va_arg(args, const char*);
    size_t l = strlen(s);
    memcpy(p, s, l);
    p += l;
  }
  va_end(args);

  *p = 0;
  return result;
}

#ifdef _WIN32

CAMLexport wchar_t* caml_stat_wcsconcat(int n, ...)
{
  va_list args;
  wchar_t *result, *p;
  size_t len = 0;
  int i;

  va_start(args, n);
  for (i = 0; i < n; i++) {
    const wchar_t *s = va_arg(args, const wchar_t*);
    len += wcslen(s);
  }
  va_end(args);

  result = caml_stat_alloc((len + 1)*sizeof(wchar_t));

  va_start(args, n);
  p = result;
  for (i = 0; i < n; i++) {
    const wchar_t *s = va_arg(args, const wchar_t*);
    size_t l = wcslen(s);
    memcpy(p, s, l*sizeof(wchar_t));
    p += l;
  }
  va_end(args);

  *p = 0;
  return result;
}

#endif
