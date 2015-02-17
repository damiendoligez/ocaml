/***********************************************************************/
/*                                                                     */
/*                                OCaml                                */
/*                                                                     */
/*         Xavier Leroy and Damien Doligez, INRIA Rocquencourt         */
/*                                                                     */
/*  Copyright 1996 Institut National de Recherche en Informatique et   */
/*  en Automatique.  All rights reserved.  This file is distributed    */
/*  under the terms of the GNU Library General Public License, with    */
/*  the special exception on linking described in file ../LICENSE.     */
/*                                                                     */
/***********************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "config.h"
#include "misc.h"
#include "memory.h"
#include "version.h"

#ifdef DEBUG

int caml_failed_assert (char * expr, char * file, int line)
{
  fprintf (stderr, "file %s; line %d ### Assertion failed: %s\n",
           file, line, expr);
  fflush (stderr);
  exit (100);
  return 1; /* not reached */
}

void caml_set_fields (char *bp, unsigned long start, unsigned long filler)
{
  mlsize_t i;
  for (i = start; i < Wosize_bp (bp); i++){
    Field (Val_bp (bp), i) = (value) filler;
  }
}

#endif /* DEBUG */

uintnat caml_verb_gc = 0;

void caml_gc_message (int level, char *msg, uintnat arg)
{
  if (level < 0 || (caml_verb_gc & level) != 0){
    fprintf (stderr, msg, arg);
    fflush (stderr);
  }
}

#ifdef DEBUG

int caml_debug_quiet = 0;

void caml_gc_debug_message (int level, char *msg, uintnat arg)
{
  if (!caml_debug_quiet) caml_gc_message (level, msg, arg);
}
#endif

CAMLexport void caml_fatal_error (char *msg)
{
  fprintf (stderr, "%s", msg);
  exit(2);
}

CAMLexport void caml_fatal_error_arg (char *fmt, char *arg)
{
  fprintf (stderr, fmt, arg);
  exit(2);
}

CAMLexport void caml_fatal_error_arg2 (char *fmt1, char *arg1,
                                       char *fmt2, char *arg2)
{
  fprintf (stderr, fmt1, arg1);
  fprintf (stderr, fmt2, arg2);
  exit(2);
}

char *caml_aligned_malloc (asize_t size, int modulo, void **block)
{
  char *raw_mem;
  uintnat aligned_mem;
                                                  Assert (modulo < Page_size);
  raw_mem = (char *) malloc (size + Page_size);
  if (raw_mem == NULL) return NULL;
  *block = raw_mem;
  raw_mem += modulo;                /* Address to be aligned */
  aligned_mem = (((uintnat) raw_mem / Page_size + 1) * Page_size);
#ifdef DEBUG
  {
    uintnat *p;
    uintnat *p0 = (void *) *block,
            *p1 = (void *) (aligned_mem - modulo),
            *p2 = (void *) (aligned_mem - modulo + size),
            *p3 = (void *) ((char *) *block + size + Page_size);

    for (p = p0; p < p1; p++) *p = Debug_filler_align;
    for (p = p1; p < p2; p++) *p = Debug_uninit_align;
    for (p = p2; p < p3; p++) *p = Debug_filler_align;
  }
#endif
  return (char *) (aligned_mem - modulo);
}

void caml_ext_table_init(struct ext_table * tbl, int init_capa)
{
  tbl->size = 0;
  tbl->capacity = init_capa;
  tbl->contents = caml_stat_alloc(sizeof(void *) * init_capa);
}

int caml_ext_table_add(struct ext_table * tbl, void * data)
{
  int res;
  if (tbl->size >= tbl->capacity) {
    tbl->capacity *= 2;
    tbl->contents =
      caml_stat_resize(tbl->contents, sizeof(void *) * tbl->capacity);
  }
  res = tbl->size;
  tbl->contents[res] = data;
  tbl->size++;
  return res;
}

void caml_ext_table_free(struct ext_table * tbl, int free_entries)
{
  int i;
  if (free_entries)
    for (i = 0; i < tbl->size; i++) caml_stat_free(tbl->contents[i]);
  caml_stat_free(tbl->contents);
}

CAMLexport char * caml_strdup(const char * s)
{
  size_t slen = strlen(s);
  char * res = caml_stat_alloc(slen + 1);
  memcpy(res, s, slen + 1);
  return res;
}

CAMLexport char * caml_strconcat(int n, ...)
{
  va_list args;
  char * res, * p;
  size_t len;
  int i;

  len = 0;
  va_start(args, n);
  for (i = 0; i < n; i++) {
    const char * s = va_arg(args, const char *);
    len += strlen(s);
  }
  va_end(args);
  res = caml_stat_alloc(len + 1);
  va_start(args, n);
  p = res;
  for (i = 0; i < n; i++) {
    const char * s = va_arg(args, const char *);
    size_t l = strlen(s);
    memcpy(p, s, l);
    p += l;
  }
  va_end(args);
  *p = 0;
  return res;
}

#ifdef CAML_TIMER
/* Timers for GC latency profiling (experimental, Linux-only) */

struct CAML_TIMER_BLOCK *CAML_TIMER_LOG = NULL;

#define GET_TIME(p,i) ((p)->ts[(i)].tv_nsec + 1000000000 * (p)->ts[(i)].tv_sec)

void CAML_TIMER_ATEXIT (void)
{
  int i;
  struct CAML_TIMER_BLOCK *p, *end_p, *start_p = NULL;
  FILE *f = NULL;
  char *fname;

  fname = getenv ("OCAML_TIMERS_FILE");
  if (fname != NULL){
    if (fname[0] == '+'){
      f = fopen (fname+1, "a");
    }else if (fname [0] == '>'){
      f = fopen (fname+1, "w");
    }else{
      f = fopen (fname, "a");
    }
  }

  if (f != NULL){
    fprintf (f, "================ OCAML LATENCY TIMERS %s\n",
             OCAML_VERSION_STRING);
    end_p = CAML_TIMER_LOG;
    for (p = CAML_TIMER_LOG; p != NULL; p = p->next){
      for (i = 0; i < p->index; i++){
        fprintf (f, "@@OCAML_TIMERS %9ld %s\n",
                 GET_TIME(p, i+1) - GET_TIME (p, i), p->tag[i+1]);
      }
      if (p->tag[0][0] != '\000'){
        fprintf (f, "@@OCAML_TIMERS %9ld %s\n",
                 GET_TIME(p, p->index) - GET_TIME (p, 0), p->tag[0]);
      }
      start_p = p;
    }
    if (start_p != NULL && end_p != NULL){
      fprintf (f, "==== start time: %18ld\n", GET_TIME(start_p, 0));
      fprintf (f, "==== end time  : %18ld\n", GET_TIME(end_p, 0));
      fprintf (f, "==== duration: %lds\n",
               (GET_TIME(end_p, 0) - GET_TIME (start_p, 0)) / 1000000000);
    }
    fflush (f);
  }
}
#endif /* CAML_TIMER */
