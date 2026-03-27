Howlang error-hint test suite

These files are intentionally invalid. Run them one by one to verify the
improved diagnostics and hints in the interpreter.

Suggested commands:
  ./howlang err_missing_rparen.how
  ./howlang err_missing_rbrace.how
  ./howlang err_single_colon_in_branch.how
  ./howlang err_misplaced_double_colon.how
  ./howlang err_missing_rparen_in_call.how
  ./howlang err_colon_outside_branch.how
  ./howlang err_bad_for_range_header.how
  ./howlang err_bad_unbounded_loop_call.how

What each file targets:
- err_missing_rparen.how: missing ')' in function parameter list
- err_missing_rbrace.how: missing '}'
- err_single_colon_in_branch.how: ':' used where '::' was likely intended
- err_misplaced_double_colon.how: '::' outside branch context
- err_missing_rparen_in_call.how: missing ')' in function call
- err_colon_outside_branch.how: stray ':'
- err_bad_for_range_header.how: malformed (i=a:b) loop header
- err_bad_unbounded_loop_call.how: malformed unbounded loop call tail
