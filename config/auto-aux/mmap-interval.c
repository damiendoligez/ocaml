/***********************************************************************/
/*                                                                     */
/*                                OCaml                                */
/*                                                                     */
/*                 Damien Doligez, Jane Street Group, LLC              */
/*                                                                     */
/*  Copyright 2015 Institut National de Recherche en Informatique et   */
/*  en Automatique.  All rights reserved.  This file is distributed    */
/*  under the terms of the GNU Library General Public License, with    */
/*  the special exception on linking described in file ../../LICENSE.  */
/*                                                                     */
/***********************************************************************/

#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>

#define reserve_size (2LL * 1024 * 1024 * 1024 * 1024)
#define huge_page_size (4 * 1024 * 1024)

int main (int argc, char *argv[]){
  void *reserve, *block;
  char *p;
  int i, res;
  reserve = mmap (NULL, reserve_size, PROT_NONE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                  -1, 0);
  if (reserve == MAP_FAILED){
    perror ("mmap (reserve)");
    return 3;
  }
  /*printf ("reserve = %p\n", reserve);*/
  /* check exclusion */
  block = mmap (reserve, huge_page_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS,
                -1, 0);
  if (block == MAP_FAILED){
    perror ("mmap (block)");
    return 3;
  }
  /*printf ("block = %p\n", block);*/
  if (block == reserve){
    fprintf (stderr, "exclusion failed\n");
    return 3;
  }
  p = (char *) block;
  for (i = 0; i < huge_page_size; i += 4096){
    p[i] = (char) i;
  }

  /* check override */
  block = mmap (reserve, huge_page_size, PROT_READ | PROT_WRITE,
                MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                -1, 0);
  if (block == MAP_FAILED){
    perror ("mmap (block)");
    return 3;
  }
  /*printf ("block = %p\n", block);*/
  if (block != reserve){
    fprintf (stderr, "override failed\n");
    return 3;
  }
  p = (char *) block;
  for (i = 0; i < huge_page_size; i += 4096){
    p[i] = (char) i;
  }
  return 0;
}
