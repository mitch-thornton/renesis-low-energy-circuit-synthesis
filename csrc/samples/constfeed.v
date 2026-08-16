// BUG-V92-01 regression fixture (v92.1): a LIVE constant -- one that feeds
// another gate.  The cover keeps it as its own block; fanout-one absorption
// must refuse to pull it into a cone interior.  Before v92.1 the C engine
// accepted that merge and died inside t_depth with no run record, on a
// DEFAULT run; Python refused it.  suite [19] maps this in both languages
// and requires byte-identical output.
module constfeed (a, b, y, z);
  input a, b;
  output y, z;
  wire c, t;
  assign c = 1'b0;
  and g1 (t, a, c);
  or  g2 (y, t, b);
  and g3 (z, a, b);
endmodule
