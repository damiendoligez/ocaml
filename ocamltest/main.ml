(**************************************************************************)
(*                                                                        *)
(*                                 OCaml                                  *)
(*                                                                        *)
(*             Sébastien Hinderer, projet Gallium, INRIA Paris           *)
(*                                                                        *)
(*   Copyright 2016 Institut National de Recherche en Informatique et     *)
(*     en Automatique.                                                    *)
(*                                                                        *)
(*   All rights reserved.  This file is distributed under the terms of    *)
(*   the GNU Lesser General Public License version 2.1, with the          *)
(*   special exception on linking described in the file LICENSE.          *)
(*                                                                        *)
(**************************************************************************)

(* Main program for the test handler *)

open Tsl_ast
open Tsl_semantics

let first_token filename =
  let input_channel = open_in filename in
  let lexbuf = Lexing.from_channel input_channel in
  Location.init lexbuf filename;
  let token =
    try Tsl_lexer.token lexbuf with e -> close_in input_channel; raise e
  in close_in input_channel; token

let is_test filename =
  match first_token filename with
    | exception e -> false
    | Tsl_parser.TSL_BEGIN -> true
    | _ -> false

let tsl_block_of_file test_filename =
  let input_channel = open_in test_filename in
  let lexbuf = Lexing.from_channel input_channel in
  Location.init lexbuf test_filename;
  match Tsl_parser.tsl_block Tsl_lexer.token lexbuf with
    | exception e -> close_in input_channel; raise e
    | _ as tsl_block -> close_in input_channel; tsl_block
  
let print_usage () =
  Printf.printf "Usage: %s testfile\n" Sys.argv.(0)

let rec run_test log path rootenv = function
  Node (testenvspec, test, subtrees) ->
  Printf.printf "Running test %s (%s) ... %!"
    path test.Tests.test_name;
  let print_test_result str = Printf.printf "%s\n%!" str in
  let testenv = interprete_environment_statements rootenv testenvspec in
  match Tests.run log testenv test with
    | Actions.Pass newenv ->
      print_test_result "passed";
      List.iteri (run_test_i log path newenv) subtrees
    | Actions.Fail _ -> print_test_result "failed"
    | Actions.Skip _ -> print_test_result "skipped"
and run_test_i log path rootenv i test_tree =
  let prefix = if path="" then "" else path ^ "." in
  let new_path = Printf.sprintf "%s%d" prefix (i+1) in
  run_test log new_path rootenv test_tree

let initial_env filename =
  Environments.add "testfile" filename Environments.empty

let run_command command = match Sys.command command with
  | 0 -> ()
  | _ as exitcode ->
    Printf.eprintf "%s failed with status %d\n%!"
      command exitcode;
    exit 2

let get_test_source_directory test_dirname =
  if not (Filename.is_relative test_dirname) then test_dirname
  else let pwd = Sys.getcwd() in
  Filename.concat pwd test_dirname

let get_test_build_directory test_dirname =
  let ocamltestdir_variable = "OCAMLTESTDIR" in
  let root = try Sys.getenv ocamltestdir_variable with
    | Not_found ->
      let default_root = "/tmp/ocamltest" in
      Printf.eprintf "The %s environment variable is not defined. Using %s.\n%!"
        ocamltestdir_variable default_root;
      default_root in
  Filename.concat root test_dirname

let make_directory dir = run_command ("mkdir -p " ^ dir)

let setup_symlinks test_source_directory test_build_directory files =
  let symlink filename =
    let src = Filename.concat test_source_directory filename in
    let cmd = "ln -s " ^ src ^" " ^ test_build_directory in
    run_command cmd in
  List.iter symlink files

let main () =
  if Array.length Sys.argv < 2 then begin
    print_usage();
    exit 1
  end;
  let test_filename = Sys.argv.(1) in
  Printf.printf "# reading test file %s\n%!" test_filename;
  let tsl_block = tsl_block_of_file test_filename in
  let (rootenv_statements, test_trees) = test_trees_of_tsl_block tsl_block in
  let test_trees = match test_trees with
    | [] ->
      let default_tests = Tests.default_tests() in
      let make_tree test = Node ([], test, []) in
      List.map make_tree default_tests
    | _ -> test_trees in
  let actions = actions_in_tests (tests_in_trees test_trees) in
  let init_env = initial_env (Filename.basename test_filename) in
  let root_environment =
    interprete_environment_statements init_env rootenv_statements in
  let rootenv = Actions.update_environment root_environment actions in
  let test_dirname = Filename.dirname test_filename in
  let test_basename = Filename.basename test_filename in
  let test_prefix = Filename.chop_extension test_basename in
  let test_source_directory = get_test_source_directory test_dirname in
  let test_build_directory = get_test_build_directory test_dirname in
  make_directory test_build_directory;
  let modules_value = Environments.safe_lookup "modules" rootenv in
  let modules = Testlib.words modules_value in
  setup_symlinks
    test_source_directory
    test_build_directory
    (test_basename::modules);
  Sys.chdir test_build_directory;
  let log_filename = test_prefix ^ ".log" in
  let log = open_out log_filename in
  List.iteri (run_test_i log "" rootenv) test_trees;
  close_out log

let _ = main()
