| Mutant | Bug | Fuzzer | Properties | Replay | Caught |
| --- | --- | --- | --- | --- | --- |
| 01_partial_fill_off_by_one | a partial fill takes one share too few | ✓ 200 | ✓ 13 | ✓ 5,006 | ✓ |
| 02_buy_limit_strict | a buy does not trade at exactly its limit | ✓ 200 | ✓ 53 | ✓ 5,119 | ✓ |
| 03_sell_limit_strict | a sell does not trade at exactly its limit | ✓ 200 | ✓ 30 | ✓ 5,006 | ✓ |
| 04_side_comparison_swapped | wrong side comparison for the limit test | ✓ 200 | ✓ 4 | ✓ 5,278 | ✓ |
| 05_walks_own_side | aggressor matches against its own side | ✓ 200 | ✓ 3 | ✓ 5,006 | ✓ |
| 06_lifo_within_level | newest order at a price fills first | ✓ 200 | ✓ 30 | ✓ 5,006 | ✓ |
| 07_stops_after_one_level | sweep never goes past the first price level | ✓ 200 | ✓ 40 | ✓ 5,278 | ✓ |
| 08_fill_at_aggressor_limit | trades print at the aggressor's limit, not the resting price | ✓ 200 | ✓ 4 | ✓ 5,278 | ✓ |
| 09_rests_full_size | remainder rests with the original size | ✓ 200 | ✓ 18 | · | ✓ |
| 10_no_rest_after_partial | a partially filled order does not rest its remainder | ✓ 200 | ✓ 18 | · | ✓ |
| 11_replace_keeps_priority | replace at the same price keeps queue position | · | ✓ 59 | ✓ 6,413 | ✓ |
| 12_partial_cancel_loses_priority | a reduced order moves to the back of its queue | ✓ 200 | ✓ 62 | ✓ 5,006 | ✓ |
| 13_level_qty_not_reduced | level total not updated on a partial reduce | · | ✓ 2 | · | ✓ |
| 14_empty_level_kept | empty price levels are never removed | ✓ 200 | ✓ 5 | ✓ 5,003 | ✓ |
| 15_count_not_incremented | level order count stops at one | ✓ 200 | ✓ 27 | ✓ 5,005 | ✓ |
| 16_unlink_head_wrong | removing the head skips the next order too | ✓ 200 | ✓ 30 | ✓ 5,006 | ✓ |
| 17_unlink_tail_stale | tail not moved back when the newest order of several leaves | ✓ 200 | ✓ 51 | ✓ | ✓ |
| 18_bids_ascending | best bid is the lowest bid | ✓ 200 | ✓ 3 | ✓ 5,006 | ✓ |
| 19_duplicate_ref_accepted | a duplicate id overwrites the live order | · | · | ✓ 63,975 | ✓ |
| 20_push_front | new orders join the front of the queue | ✓ 200 | ✓ 27 | ✓ 5,006 | ✓ |
| 21_overfill_unchecked | executions larger than the order are not caught | · | · | ✓ 106,630 | ✓ |
| 22_cancel_deletes | a partial cancel deletes the whole order | · | · | ✓ 201 | ✓ |
| 23_exec_short_by_one | executions remove one share too few | · | · | ✓ 5,181 | ✓ |
| 24_halt_state_ignored | halts and quote-only periods count as continuous trading | · | · | ✓ 88,917 | ✓ |
| 25_add_price_offset | Add price read one byte early | · | · | ✓ 5,005 | ✓ |
| 26_replace_fields_swapped | Replace shares and price swapped | · | · | ✓ 5,108 | ✓ |
| 27_remainder_side_wrong | posted remainder looked for on the resting side | · | · | ✓ 213,238 | ✓ |
| 28_replace_flips_side | replace submits the new order on the other side | ✓ 200 | · | · | ✓ |

**28 of 28 seeded bugs caught.** Fuzzer 18, properties 19, replay 24; 10 caught by exactly one validator. Median operations to detection: fuzzer 200, properties 27. Cells show operations (or ITCH messages, for replay) until the first failure; · means not caught.
