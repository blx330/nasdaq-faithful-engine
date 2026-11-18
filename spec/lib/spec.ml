(* Executable specification of a price-time priority limit order book.

   This is the reference the C++ engine must agree with. It is written for
   clarity, not speed: immutable maps from price to a FIFO list of orders.

   Rules it defines:
   - An incoming limit order trades against the opposite side, best price
     first, oldest order first within a price, up to its limit. Each trade
     is at the resting order's price. Any remainder rests at the back of the
     queue at its limit.
   - A partial cancel reduces an order in place: it keeps its queue position.
   - A replace removes the old order and submits the new one as a fresh
     order: it loses priority, even at the same price and size.
   - Unknown or duplicate ids, and non-positive sizes, are rejected and leave
     the book unchanged. *)

module IntMap = Map.Make (Int)

type side = Buy | Sell

type order = { id : int; qty : int }

type book = {
  bids : order list IntMap.t;  (* price -> FIFO, oldest first *)
  asks : order list IntMap.t;
  index : (side * int) IntMap.t;  (* id -> side and price, for cancels *)
}

type fill = { aggressor : int; resting : int; price : int; fqty : int }

type event = Fill of fill | Reject of int

let empty = { bids = IntMap.empty; asks = IntMap.empty; index = IntMap.empty }

let opposite = function Buy -> Sell | Sell -> Buy

let side_map b = function Buy -> b.bids | Sell -> b.asks

let with_side b side m =
  match side with Buy -> { b with bids = m } | Sell -> { b with asks = m }

(* Best price on a side: highest bid, lowest ask. *)
let best b side =
  let m = side_map b side in
  match side with Buy -> IntMap.max_binding_opt m | Sell -> IntMap.min_binding_opt m

let crosses aggr_side ~limit price =
  match aggr_side with Buy -> price <= limit | Sell -> price >= limit

let rest b side price o =
  let m = side_map b side in
  let q = Option.value ~default:[] (IntMap.find_opt price m) in
  let b = with_side b side (IntMap.add price (q @ [ o ]) m) in
  { b with index = IntMap.add o.id (side, price) b.index }

(* Trade the aggressor against the opposite side until it is filled or no
   longer crosses. Returns the new book, fills in order, and the remainder. *)
let rec sweep b ~aggr_id ~side ~limit ~left acc =
  if left = 0 then (b, List.rev acc, 0)
  else
    let opp = opposite side in
    match best b opp with
    | Some (price, queue) when crosses side ~limit price -> (
        match queue with
        | [] -> assert false (* empty levels are never stored *)
        | o :: rest_q ->
            let q = min o.qty left in
            let fill = Fill { aggressor = aggr_id; resting = o.id; price; fqty = q } in
            let m = side_map b opp in
            let b =
              if q = o.qty then
                let m = if rest_q = [] then IntMap.remove price m else IntMap.add price rest_q m in
                { (with_side b opp m) with index = IntMap.remove o.id b.index }
              else with_side b opp (IntMap.add price ({ o with qty = o.qty - q } :: rest_q) m)
            in
            sweep b ~aggr_id ~side ~limit ~left:(left - q) (fill :: acc))
    | _ -> (b, List.rev acc, left)

let submit b ~id ~side ~price ~qty =
  if qty <= 0 || price <= 0 || IntMap.mem id b.index then (b, [ Reject id ])
  else
    let b, fills, left = sweep b ~aggr_id:id ~side ~limit:price ~left:qty [] in
    let b = if left > 0 then rest b side price { id; qty = left } else b in
    (b, fills)

let remove_order b id =
  match IntMap.find_opt id b.index with
  | None -> None
  | Some (side, price) ->
      let m = side_map b side in
      let q = List.filter (fun o -> o.id <> id) (IntMap.find price m) in
      let m = if q = [] then IntMap.remove price m else IntMap.add price q m in
      Some ({ (with_side b side m) with index = IntMap.remove id b.index }, side)

let delete b ~id =
  match remove_order b id with None -> (b, [ Reject id ]) | Some (b, _) -> (b, [])

(* Cancel [qty] shares. Cancelling the whole remainder (or more) deletes. *)
let cancel b ~id ~qty =
  match IntMap.find_opt id b.index with
  | None -> (b, [ Reject id ])
  | Some _ when qty <= 0 -> (b, [ Reject id ])
  | Some (side, price) ->
      let m = side_map b side in
      let q = IntMap.find price m in
      let o = List.find (fun o -> o.id = id) q in
      if qty >= o.qty then delete b ~id
      else
        let q = List.map (fun x -> if x.id = id then { x with qty = x.qty - qty } else x) q in
        (with_side b side (IntMap.add price q m), [])

let replace b ~old_id ~new_id ~price ~qty =
  if qty <= 0 || price <= 0 || (IntMap.mem new_id b.index && new_id <> old_id) then (b, [ Reject old_id ])
  else
    match remove_order b old_id with
    | None -> (b, [ Reject old_id ])
    | Some (b, side) -> submit b ~id:new_id ~side ~price ~qty

(* Levels best first, each as (price, orders oldest first). *)
let levels b side =
  let l = IntMap.bindings (side_map b side) in
  match side with Buy -> List.rev l | Sell -> l

let total_qty b =
  let sum m = IntMap.fold (fun _ q acc -> List.fold_left (fun a o -> a + o.qty) acc q) m 0 in
  sum b.bids + sum b.asks

let is_crossed b =
  match (best b Buy, best b Sell) with
  | Some (bp, _), Some (ap, _) -> bp >= ap
  | _ -> false

(* ---- the shared text protocol ---- *)

let side_of_string = function "B" -> Buy | "S" -> Sell | s -> failwith ("bad side " ^ s)
let string_of_side = function Buy -> "B" | Sell -> "S"

let render_event = function
  | Fill f -> Printf.sprintf "T %d %d %d %d" f.aggressor f.resting f.price f.fqty
  | Reject id -> Printf.sprintf "R %d" id

let render_book b =
  let line side (price, q) =
    "L " ^ string_of_side side ^ " " ^ string_of_int price
    ^ String.concat "" (List.map (fun o -> Printf.sprintf " %d:%d" o.id o.qty) q)
  in
  List.map (line Buy) (levels b Buy) @ List.map (line Sell) (levels b Sell)

(* Apply one command line. Returns the new book and output lines. *)
let step b line =
  match String.split_on_char ' ' (String.trim line) with
  | [ "A"; id; s; p; q ] ->
      let b, ev = submit b ~id:(int_of_string id) ~side:(side_of_string s) ~price:(int_of_string p) ~qty:(int_of_string q) in
      (b, List.map render_event ev)
  | [ "X"; id; q ] ->
      let b, ev = cancel b ~id:(int_of_string id) ~qty:(int_of_string q) in
      (b, List.map render_event ev)
  | [ "D"; id ] ->
      let b, ev = delete b ~id:(int_of_string id) in
      (b, List.map render_event ev)
  | [ "U"; o; n; p; q ] ->
      let b, ev = replace b ~old_id:(int_of_string o) ~new_id:(int_of_string n) ~price:(int_of_string p) ~qty:(int_of_string q) in
      (b, List.map render_event ev)
  | [ "#" ] -> (empty, render_book b @ [ "#" ])
  | [ "" ] -> (b, [])
  | _ -> failwith ("bad command: " ^ line)
