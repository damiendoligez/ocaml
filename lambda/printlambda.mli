(**************************************************************************)
(*                                                                        *)
(*                                 OCaml                                  *)
(*                                                                        *)
(*             Xavier Leroy, projet Cristal, INRIA Rocquencourt           *)
(*                                                                        *)
(*   Copyright 1996 Institut National de Recherche en Informatique et     *)
(*     en Automatique.                                                    *)
(*                                                                        *)
(*   All rights reserved.  This file is distributed under the terms of    *)
(*   the GNU Lesser General Public License version 2.1, with the          *)
(*   special exception on linking described in the file LICENSE.          *)
(*                                                                        *)
(**************************************************************************)

open Lambda

open Format

val integer_comparison: formatter -> integer_comparison -> unit
val float_comparison: formatter -> float_comparison -> unit
val structured_constant: formatter -> structured_constant -> unit
val lambda: formatter -> lambda -> unit
val program: formatter -> program -> unit
val primitive: formatter -> primitive -> unit
val name_of_primitive : primitive -> string
val value_kind : formatter -> value_kind -> unit
val value_kind' : formatter -> value_kind -> unit
val meth_kind : formatter -> meth_kind -> unit
val block_shape : formatter -> value_kind list option -> unit
val record_rep : formatter -> Types.record_representation -> unit
val print_bigarray :
  string -> bool -> Lambda.bigarray_kind -> formatter ->
  Lambda.bigarray_layout -> unit
val function_attribute : formatter -> function_attribute -> unit
val apply_tailcall_attribute : formatter -> bool -> unit
val apply_inlined_attribute : formatter -> inline_attribute -> unit
val apply_specialised_attribute : formatter -> specialise_attribute -> unit
