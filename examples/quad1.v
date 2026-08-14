module quad1 (x0, x1, x2, x3, x4, x5, x6, x7, q);
input x0, x1, x2, x3, x4, x5, x6, x7;
output q;
wire a0, a1, a2, t0, t1;
and (a0, x0, x1);
and (a1, x2, x3);
and (a2, x4, x5);
xor (t0, a0, a1);
xor (t1, t0, a2);
xor (q, t1, x6);
endmodule
