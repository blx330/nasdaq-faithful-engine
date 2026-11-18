(* Reads protocol commands on stdin, writes events and book dumps on stdout.
   A "#" line dumps the book, prints "#", and starts a fresh book; the book
   is also dumped at end of input. *)
let () =
  let b = ref Spec.empty in
  let out = Buffer.create (1 lsl 16) in
  (try
     while true do
       let line = input_line stdin in
       let nb, lines = Spec.step !b line in
       b := nb;
       List.iter (fun l -> Buffer.add_string out l; Buffer.add_char out '\n') lines;
       if Buffer.length out > 1 lsl 20 then (print_string (Buffer.contents out); Buffer.clear out)
     done
   with End_of_file -> ());
  List.iter (fun l -> Buffer.add_string out l; Buffer.add_char out '\n') (Spec.render_book !b);
  print_string (Buffer.contents out)
