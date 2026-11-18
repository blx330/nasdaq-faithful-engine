(* QCheck properties on the executable spec itself. If the spec is wrong,
   agreeing with it proves nothing, so the spec gets its own tests. *)
open Spec

type cmd =
  | Sub of int * side * int * int
  | Can of int * int
  | Del of int
  | Rep of int * int * int * int

(* Ids 1..n are assigned in order; commands refer to earlier ids. Prices
   cluster on a few ticks so queues build up and orders cross often. *)
let gen_cmds =
  let open QCheck.Gen in
  let* n = int_range 1 120 in
  let rec go i next acc =
    if i = n then return (List.rev acc)
    else
      let* k = int_range 0 9 in
      let* side = oneofl [ Buy; Sell ] in
      let* price = int_range 98 102 in
      let* qty = int_range 1 5 in
      let* tgt = int_range 1 (max 1 (next - 1)) in
      let c =
        if k < 6 || next = 1 then Sub (next, side, price, qty * 100)
        else if k < 7 then Can (tgt, qty * 50)
        else if k < 8 then Del tgt
        else Rep (tgt, next, price, qty * 100)
      in
      let next = match c with Sub _ | Rep _ -> next + 1 | _ -> next in
      go (i + 1) next (c :: acc)
  in
  go 0 1 []

let show = function
  | Sub (i, s, p, q) -> Printf.sprintf "A %d %s %d %d" i (string_of_side s) p q
  | Can (i, q) -> Printf.sprintf "X %d %d" i q
  | Del i -> Printf.sprintf "D %d" i
  | Rep (o, n, p, q) -> Printf.sprintf "U %d %d %d %d" o n p q

let arb = QCheck.make ~print:(fun l -> String.concat "\n" (List.map show l)) gen_cmds

let apply b = function
  | Sub (id, side, price, qty) -> submit b ~id ~side ~price ~qty
  | Can (id, qty) -> cancel b ~id ~qty
  | Del id -> delete b ~id
  | Rep (o, n, price, qty) -> replace b ~old_id:o ~new_id:n ~price ~qty

let run cmds = List.fold_left (fun (b, evs) c -> let b, e = apply b c in (b, evs @ [ (c, e) ])) (empty, []) cmds

let never_crossed =
  QCheck.Test.make ~count:2000 ~name:"book is never crossed after a command" arb (fun cmds ->
      let _, ok =
        List.fold_left (fun (b, ok) c -> let b, _ = apply b c in (b, ok && not (is_crossed b))) (empty, true) cmds
      in
      ok)

let qty_of b id =
  match IntMap.find_opt id b.index with
  | None -> 0
  | Some (side, price) ->
      (List.find (fun o -> o.id = id) (IntMap.find price (side_map b side))).qty

(* Every accepted command changes resting shares by exactly what it should:
   a submit adds q and removes 2 * traded (the resting side's shares, and the
   aggressor's shares that never rest); a replace first removes the old
   order; cancels and deletes remove what they say. *)
let conservation =
  QCheck.Test.make ~count:2000 ~name:"shares are conserved by every command" arb (fun cmds ->
      let _, ok =
        List.fold_left
          (fun (b, ok) c ->
            let before = total_qty b in
            let nb, e = apply b c in
            let after = total_qty nb in
            let rejected = List.exists (function Reject _ -> true | _ -> false) e in
            let t = List.fold_left (fun a -> function Fill f -> a + f.fqty | _ -> a) 0 e in
            let good =
              if rejected then after = before && nb == b
              else
                match c with
                | Sub (_, _, _, q) -> after = before + q - (2 * t)
                | Rep (o, _, _, q) -> after = before - qty_of b o + q - (2 * t)
                | Can (id, q) -> after = before - min q (qty_of b id)
                | Del id -> after = before - qty_of b id
            in
            (nb, ok && good))
          (empty, true) cmds
      in
      ok)

(* A buy walks asks upward and a sell walks bids downward, and every fill is
   within the aggressor's limit. *)
let fills_ordered =
  QCheck.Test.make ~count:2000 ~name:"fills are price-ordered and within the limit" arb (fun cmds ->
      let _, evs = run cmds in
      List.for_all
        (fun (c, e) ->
          match c with
          | Sub (_, side, limit, _) ->
              let ps = List.filter_map (function Fill f -> Some f.price | _ -> None) e in
              let rec mono = function
                | a :: (b :: _ as t) -> (if side = Buy then a <= b else a >= b) && mono t
                | _ -> true
              in
              mono ps && List.for_all (fun p -> crosses side ~limit p) ps
          | _ -> true)
        evs)

(* Concrete priority rules, checked on small fixed books. *)
let fill_ids evs = List.filter_map (function Fill f -> Some f.resting | _ -> None) evs

let unit_rules () =
  let b = empty in
  let b, _ = submit b ~id:1 ~side:Sell ~price:100 ~qty:100 in
  let b, _ = submit b ~id:2 ~side:Sell ~price:100 ~qty:100 in
  (* partial cancel keeps priority *)
  let b1, _ = cancel b ~id:1 ~qty:50 in
  let _, e = submit b1 ~id:3 ~side:Buy ~price:100 ~qty:60 in
  assert (fill_ids e = [ 1; 2 ]);
  (* replace at the same price and size loses priority *)
  let b2, _ = replace b ~old_id:1 ~new_id:4 ~price:100 ~qty:100 in
  let _, e = submit b2 ~id:5 ~side:Buy ~price:100 ~qty:100 in
  assert (fill_ids e = [ 2 ]);
  (* better price beats earlier time *)
  let b3, _ = submit b ~id:6 ~side:Sell ~price:99 ~qty:100 in
  let _, e = submit b3 ~id:7 ~side:Buy ~price:100 ~qty:150 in
  assert (fill_ids e = [ 6; 1 ]);
  print_endline "unit rules: ok"

let () =
  unit_rules ();
  QCheck_base_runner.run_tests_main [ never_crossed; conservation; fills_ordered ]
