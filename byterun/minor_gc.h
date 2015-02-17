/***********************************************************************/
/*                                                                     */
/*                                OCaml                                */
/*                                                                     */
/*             Damien Doligez, projet Para, INRIA Rocquencourt         */
/*                                                                     */
/*  Copyright 1996 Institut National de Recherche en Informatique et   */
/*  en Automatique.  All rights reserved.  This file is distributed    */
/*  under the terms of the GNU Library General Public License, with    */
/*  the special exception on linking described in file ../LICENSE.     */
/*                                                                     */
/***********************************************************************/

#ifndef CAML_MINOR_GC_H
#define CAML_MINOR_GC_H


#include "misc.h"

CAMLextern value *caml_young_start, *caml_young_end;
CAMLextern value *caml_young_alloc_start, *caml_young_alloc_end;
CAMLextern value *caml_young_ptr, *caml_young_limit;
CAMLextern value *caml_young_trigger;

CAMLexport value *caml_young_aging_start = NULL,
                 *caml_young_aging_semi = NULL,
                 *caml_young_aging_end = NULL;
CAMLexport value *caml_young_aging_ptr = NULL;
CAMLexport int caml_young_aging_phase = 0;

extern asize_t caml_minor_heap_wsz, caml_minor_aging_wsz;
extern int caml_in_minor_collection;

struct caml_ref_table {
  value **base;
  value **end;
  value **threshold;
  value **ptr;
  value **limit;
  asize_t size;
  asize_t reserve;
};
CAMLextern struct caml_ref_table caml_ref_table, caml_weak_ref_table;

#define Is_young(val) \
  (Assert (Is_block (val)), \
   (addr)(val) < (addr)caml_young_end && (addr)(val) > (addr)caml_young_start)

extern void caml_set_minor_heap_size (asize_t); /* size in bytes */
extern void caml_empty_minor_heap (void);
CAMLextern void caml_gc_dispatch (void);
CAMLextern void garbage_collection (void); /* def in asmrun/signals.c */
extern void caml_realloc_ref_table (struct caml_ref_table *);
extern void caml_alloc_table (struct caml_ref_table *, asize_t, asize_t);
extern void caml_oldify_one (value, value *);
extern void caml_oldify_mopup (void);

#define Oldify(p) do{ \
    value __oldify__v__ = *p; \
    if (Is_block (__oldify__v__) && Is_young (__oldify__v__)){ \
      caml_oldify_one (__oldify__v__, (p)); \
    } \
  }while(0)

#endif /* CAML_MINOR_GC_H */
